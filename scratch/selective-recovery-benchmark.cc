#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ppp-header.h"
#include "ns3/tcp-socket-base.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

using namespace ns3;

class ControlledInitialLossErrorModel : public ErrorModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::ControlledInitialLossErrorModel")
                .SetParent<ErrorModel>()
                .AddConstructor<ControlledInitialLossErrorModel>();

        return tid;
    }

    void SetLossRate(double lossRate)
    {
        NS_ABORT_MSG_IF(lossRate < 0.0 || lossRate > 1.0,
                        "Loss rate must be in [0,1]");
        m_lossRate = lossRate;
    }

    void SetSeed(uint32_t seed)
    {
        m_seed = seed;
    }

    uint64_t GetDroppedBytes() const
    {
        return m_droppedBytes;
    }

    uint64_t GetDroppedCriticalBytes() const
    {
        return m_droppedCriticalBytes;
    }

    uint64_t GetDroppedNonCriticalBytes() const
    {
        return m_droppedNonCriticalBytes;
    }

    uint32_t GetDroppedSegments() const
    {
        return m_droppedSegments;
    }

    uint32_t GetInitialDataSegments() const
    {
        return m_initialDataSegments;
    }

  private:
    bool ShouldDrop(uint32_t seq) const
    {
        uint64_t x =
            static_cast<uint64_t>(seq) ^
            (static_cast<uint64_t>(m_seed) *
             UINT64_C(0x9e3779b97f4a7c15));

        x ^= x >> 33;
        x *= UINT64_C(0xff51afd7ed558ccd);
        x ^= x >> 33;
        x *= UINT64_C(0xc4ceb9fe1a85ec53);
        x ^= x >> 33;

        const long double normalized =
            static_cast<long double>(x) /
            (static_cast<long double>(
                 std::numeric_limits<uint64_t>::max()) +
             1.0L);

        return normalized <
               static_cast<long double>(m_lossRate);
    }

    bool DoCorrupt(Ptr<Packet> packet) override
    {
        Ptr<Packet> copy = packet->Copy();

        PppHeader ppp;
        if (copy->RemoveHeader(ppp) == 0)
        {
            return false;
        }

        Ipv4Header ipv4;
        if (copy->RemoveHeader(ipv4) == 0)
        {
            return false;
        }

        if (ipv4.GetProtocol() != 6)
        {
            return false;
        }

        TcpHeader tcp;
        if (copy->RemoveHeader(tcp) == 0)
        {
            return false;
        }

        const uint32_t payloadSize = copy->GetSize();

        if (payloadSize == 0)
        {
            return false;
        }

        const uint32_t seq =
            tcp.GetSequenceNumber().GetValue();

        // Only the first transmission of each starting sequence number
        // participates in controlled loss. Retransmissions are not
        // artificially lost in this pilot.
        if (!m_seenSequences.insert(seq).second)
        {
            return false;
        }

        ++m_initialDataSegments;

        if (!ShouldDrop(seq))
        {
            return false;
        }

        const bool critical =
            (tcp.GetFlags() & TcpHeader::URG) != 0;

        ++m_droppedSegments;
        m_droppedBytes += payloadSize;

        if (critical)
        {
            m_droppedCriticalBytes += payloadSize;
        }
        else
        {
            m_droppedNonCriticalBytes += payloadSize;
        }

        return true;
    }

    void DoReset() override
    {
        m_seenSequences.clear();
        m_initialDataSegments = 0;
        m_droppedSegments = 0;
        m_droppedBytes = 0;
        m_droppedCriticalBytes = 0;
        m_droppedNonCriticalBytes = 0;
    }

    double m_lossRate{0.0};
    uint32_t m_seed{1};

    std::unordered_set<uint32_t> m_seenSequences;

    uint32_t m_initialDataSegments{0};
    uint32_t m_droppedSegments{0};

    uint64_t m_droppedBytes{0};
    uint64_t m_droppedCriticalBytes{0};
    uint64_t m_droppedNonCriticalBytes{0};
};

NS_OBJECT_ENSURE_REGISTERED(ControlledInitialLossErrorModel);

static Ptr<Socket> g_serverSocket;
static std::vector<Ptr<Socket>> g_acceptedSockets;

static bool g_selective = false;
static double g_lossRate = 0.03;
static uint32_t g_criticalPercent = 50;
static uint32_t g_seed = 1;

static uint32_t g_recordCount = 120;
static uint32_t g_recordSize = 400;

static uint32_t g_nextRecordIndex = 0;

static uint64_t g_totalSentBytes = 0;
static uint64_t g_expectedCriticalBytes = 0;

static uint64_t g_receivedBytes = 0;
static uint64_t g_receivedCriticalBytes = 0;
static uint64_t g_receivedNonCriticalBytes = 0;
static uint64_t g_unexpectedBytes = 0;

static uint64_t g_retransmittedBytes = 0;
static uint64_t g_criticalRetransmittedBytes = 0;
static uint64_t g_nonCriticalRetransmittedBytes = 0;

static uint64_t g_skipBytes = 0;
static uint32_t g_skipControls = 0;

static Time g_sendStartTime;
static Time g_criticalCompletionTime;
static Time g_lastDeliveryTime;

static bool g_criticalComplete = false;

static bool
IsCriticalRecord(uint32_t index, uint32_t criticalPercent)
{
    if (criticalPercent == 0)
    {
        return false;
    }

    if (criticalPercent >= 100)
    {
        return true;
    }

    return ((index * criticalPercent) % 100) <
           criticalPercent;
}

static void
TxTrace(Ptr<const Packet> packet,
        const TcpHeader& header,
        Ptr<const TcpSocketBase> socket)
{
    const uint8_t flags = header.GetFlags();

    const bool skipControl =
        packet->GetSize() == 0 &&
        (flags & (TcpHeader::ACK | TcpHeader::URG)) ==
            (TcpHeader::ACK | TcpHeader::URG) &&
        header.GetUrgentPointer() > 0;

    if (!skipControl)
    {
        return;
    }

    ++g_skipControls;
    g_skipBytes += header.GetUrgentPointer();
}

static void
RetransmissionTrace(Ptr<const Packet> packet,
                    const TcpHeader& header,
                    const Address& local,
                    const Address& peer,
                    Ptr<const TcpSocketBase> socket)
{
    const uint64_t bytes = packet->GetSize();

    g_retransmittedBytes += bytes;

    const bool critical =
        (header.GetFlags() & TcpHeader::URG) != 0;

    if (critical)
    {
        g_criticalRetransmittedBytes += bytes;
    }
    else
    {
        g_nonCriticalRetransmittedBytes += bytes;
    }
}

static void
ReceiveData(Ptr<Socket> socket)
{
    while (Ptr<Packet> packet = socket->Recv())
    {
        const uint32_t size = packet->GetSize();

        if (size == 0)
        {
            break;
        }

        std::vector<uint8_t> data(size);

        const uint32_t copied =
            packet->CopyData(data.data(), size);

        NS_ABORT_MSG_IF(copied != size,
                        "Failed to copy application data");

        for (uint8_t byte : data)
        {
            if (byte == 'C')
            {
                ++g_receivedCriticalBytes;
            }
            else if (byte == 'N')
            {
                ++g_receivedNonCriticalBytes;
            }
            else
            {
                ++g_unexpectedBytes;
            }
        }

        g_receivedBytes += size;
        g_lastDeliveryTime = Simulator::Now();

        if (!g_criticalComplete &&
            g_receivedCriticalBytes ==
                g_expectedCriticalBytes)
        {
            g_criticalComplete = true;
            g_criticalCompletionTime =
                Simulator::Now();
        }
    }
}

static bool
AcceptRequest(Ptr<Socket> socket,
              const Address& from)
{
    return true;
}

static void
AcceptConnection(Ptr<Socket> socket,
                 const Address& from)
{
    g_acceptedSockets.push_back(socket);

    socket->SetRecvCallback(
        MakeCallback(&ReceiveData));
}

static void
PumpApplicationData(Ptr<Socket> socket)
{
    while (g_nextRecordIndex < g_recordCount)
    {
        // Preserve one application record as one Send() operation.
        // Wait until the TCP transmit buffer can accept the whole record.
        if (socket->GetTxAvailable() < g_recordSize)
        {
            return;
        }

        const uint32_t i =
            g_nextRecordIndex;

        const bool critical =
            IsCriticalRecord(
                i,
                g_criticalPercent);

        const uint8_t marker =
            critical ? 'C' : 'N';

        const uint32_t sendFlags =
            critical ? 1 : 0;

        std::vector<uint8_t> payload(
            g_recordSize,
            marker);

        Ptr<Packet> packet =
            Create<Packet>(
                payload.data(),
                static_cast<uint32_t>(
                    payload.size()));

        const int result =
            socket->Send(
                packet,
                sendFlags);

        if (result < 0)
        {
            // The socket is asynchronous. Wait for SendReady()
            // instead of treating temporary backpressure as fatal.
            return;
        }

        NS_ABORT_MSG_IF(
            result !=
                static_cast<int>(g_recordSize),
            "Unexpected partial application record acceptance");

        ++g_nextRecordIndex;
    }
}

static void
SendReady(Ptr<Socket> socket,
          uint32_t available)
{
    (void)available;

    PumpApplicationData(socket);
}

static void
ConnectionSucceeded(Ptr<Socket> socket)
{
    Ptr<TcpSocketBase> tcp =
        DynamicCast<TcpSocketBase>(socket);

    NS_ABORT_MSG_IF(
        tcp == nullptr,
        "Client socket is not TcpSocketBase");

    tcp->SetSelectiveRecoveryEnabled(
        g_selective);

    g_sendStartTime =
        Simulator::Now();

    g_nextRecordIndex = 0;

    socket->SetSendCallback(
        MakeCallback(&SendReady));

    PumpApplicationData(socket);
}

static void
ConnectionFailed(Ptr<Socket> socket)
{
    NS_FATAL_ERROR("TCP connection failed");
}

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);

    cmd.AddValue(
        "selective",
        "Enable selective recovery",
        g_selective);

    cmd.AddValue(
        "lossRate",
        "Controlled first-transmission loss rate",
        g_lossRate);

    cmd.AddValue(
        "criticalPercent",
        "Percentage of application bytes marked critical",
        g_criticalPercent);

    cmd.AddValue(
        "seed",
        "Deterministic loss-pattern seed",
        g_seed);

    cmd.AddValue(
        "records",
        "Number of application records",
        g_recordCount);

    cmd.AddValue(
        "recordSize",
        "Bytes per application record",
        g_recordSize);

    cmd.Parse(argc, argv);

    NS_ABORT_MSG_IF(
        g_criticalPercent > 100,
        "criticalPercent must be <= 100");

    g_totalSentBytes =
        static_cast<uint64_t>(g_recordCount) *
        g_recordSize;

    uint64_t criticalRecords = 0;

    for (uint32_t i = 0;
         i < g_recordCount;
         ++i)
    {
        if (IsCriticalRecord(
                i,
                g_criticalPercent))
        {
            ++criticalRecords;
        }
    }

    g_expectedCriticalBytes =
        criticalRecords *
        g_recordSize;

    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;

    p2p.SetDeviceAttribute(
        "DataRate",
        StringValue("10Mbps"));

    p2p.SetChannelAttribute(
        "Delay",
        StringValue("10ms"));

    NetDeviceContainer devices =
        p2p.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper ipv4;

    ipv4.SetBase(
        "10.1.1.0",
        "255.255.255.0");

    Ipv4InterfaceContainer interfaces =
        ipv4.Assign(devices);

    Ptr<ControlledInitialLossErrorModel> lossModel =
        CreateObject<
            ControlledInitialLossErrorModel>();

    lossModel->SetLossRate(g_lossRate);
    lossModel->SetSeed(g_seed);

    Ptr<PointToPointNetDevice> receiverDevice =
        DynamicCast<PointToPointNetDevice>(
            devices.Get(1));

    NS_ABORT_MSG_IF(
        receiverDevice == nullptr,
        "Receiver device cast failed");

    receiverDevice->SetReceiveErrorModel(
        lossModel);

    const uint16_t port = 5000;

    g_serverSocket =
        Socket::CreateSocket(
            nodes.Get(1),
            TcpSocketFactory::GetTypeId());

    g_serverSocket->Bind(
        InetSocketAddress(
            Ipv4Address::GetAny(),
            port));

    g_serverSocket->Listen();

    g_serverSocket->SetAcceptCallback(
        MakeCallback(&AcceptRequest),
        MakeCallback(&AcceptConnection));

    Ptr<Socket> client =
        Socket::CreateSocket(
            nodes.Get(0),
            TcpSocketFactory::GetTypeId());

    Ptr<TcpSocketBase> tcpClient =
        DynamicCast<TcpSocketBase>(client);

    NS_ABORT_MSG_IF(
        tcpClient == nullptr,
        "Client socket cast failed");

    tcpClient->TraceConnectWithoutContext(
        "Tx",
        MakeCallback(&TxTrace));

    tcpClient->TraceConnectWithoutContext(
        "Retransmission",
        MakeCallback(&RetransmissionTrace));

    client->SetConnectCallback(
        MakeCallback(&ConnectionSucceeded),
        MakeCallback(&ConnectionFailed));

    Simulator::Schedule(
        Seconds(0.1),
        &Socket::Connect,
        client,
        InetSocketAddress(
            interfaces.GetAddress(1),
            port));

    Simulator::Stop(Seconds(30.0));

    Simulator::Run();
    Simulator::Destroy();

    const uint64_t expectedReceivedBytes =
        g_selective
            ? g_totalSentBytes -
                  lossModel->
                      GetDroppedNonCriticalBytes()
            : g_totalSentBytes;

    const bool criticalDeliveryPass =
        g_receivedCriticalBytes ==
        g_expectedCriticalBytes;

    const bool totalDeliveryPass =
        g_receivedBytes ==
        expectedReceivedBytes;

    const bool contentPass =
        g_unexpectedBytes == 0;

    const bool recoveryPolicyPass =
        !g_selective ||
        g_nonCriticalRetransmittedBytes == 0;

    const bool baselinePolicyPass =
        g_selective ||
        g_skipControls == 0;

    const bool applicationQueuePass =
        g_nextRecordIndex == g_recordCount;

    const bool valid =
        applicationQueuePass &&
        criticalDeliveryPass &&
        totalDeliveryPass &&
        contentPass &&
        recoveryPolicyPass &&
        baselinePolicyPass &&
        g_criticalComplete;

    double criticalCompletionMs = -1.0;
    double lastDeliveryMs = -1.0;

    if (g_criticalComplete)
    {
        criticalCompletionMs =
            (g_criticalCompletionTime -
             g_sendStartTime)
                .GetSeconds() *
            1000.0;
    }

    if (g_receivedBytes > 0)
    {
        lastDeliveryMs =
            (g_lastDeliveryTime -
             g_sendStartTime)
                .GetSeconds() *
            1000.0;
    }

    double criticalGoodputMbps = 0.0;
    double deliveredGoodputMbps = 0.0;

    if (criticalCompletionMs > 0.0)
    {
        criticalGoodputMbps =
            static_cast<double>(
                g_expectedCriticalBytes) *
            8.0 /
            (criticalCompletionMs / 1000.0) /
            1e6;
    }

    if (lastDeliveryMs > 0.0)
    {
        deliveredGoodputMbps =
            static_cast<double>(
                g_receivedBytes) *
            8.0 /
            (lastDeliveryMs / 1000.0) /
            1e6;
    }

    std::cout << std::fixed
              << std::setprecision(6);

    std::cout
        << "RESULT"
        << " mode="
        << (g_selective
                ? "selective"
                : "baseline")
        << " loss=" << g_lossRate
        << " critical="
        << g_criticalPercent
        << " seed=" << g_seed
        << " sent="
        << g_totalSentBytes
        << " received="
        << g_receivedBytes
        << " critical_expected="
        << g_expectedCriticalBytes
        << " critical_received="
        << g_receivedCriticalBytes
        << " dropped_segments="
        << lossModel->GetDroppedSegments()
        << " dropped_bytes="
        << lossModel->GetDroppedBytes()
        << " dropped_critical="
        << lossModel->
               GetDroppedCriticalBytes()
        << " dropped_noncritical="
        << lossModel->
               GetDroppedNonCriticalBytes()
        << " retx_bytes="
        << g_retransmittedBytes
        << " retx_critical="
        << g_criticalRetransmittedBytes
        << " retx_noncritical="
        << g_nonCriticalRetransmittedBytes
        << " skip_bytes="
        << g_skipBytes
        << " skip_controls="
        << g_skipControls
        << " critical_completion_ms="
        << criticalCompletionMs
        << " last_delivery_ms="
        << lastDeliveryMs
        << " critical_goodput_mbps="
        << criticalGoodputMbps
        << " delivered_goodput_mbps="
        << deliveredGoodputMbps
        << " unexpected="
        << g_unexpectedBytes
        << " valid="
        << (valid ? 1 : 0)
        << std::endl;

    return valid ? 0 : 1;
}
