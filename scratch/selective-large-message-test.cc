#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ppp-header.h"
#include "ns3/tcp-socket-base.h"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace ns3;

class DropMiddleNonCriticalSegment : public ErrorModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::DropMiddleNonCriticalSegment")
                .SetParent<ErrorModel>()
                .AddConstructor<DropMiddleNonCriticalSegment>();

        return tid;
    }

    bool WasDropped() const
    {
        return m_dropped;
    }

    uint32_t DroppedBytes() const
    {
        return m_droppedBytes;
    }

  private:
    bool DoCorrupt(Ptr<Packet> packet) override
    {
        if (m_dropped)
        {
            return false;
        }

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

        const bool critical =
            (tcp.GetFlags() & TcpHeader::URG) != 0;

        if (critical)
        {
            return false;
        }

        ++m_nonCriticalSegmentsSeen;

        // Drop the second TCP segment belonging to the large
        // non-critical application send.
        if (m_nonCriticalSegmentsSeen == 2)
        {
            m_dropped = true;
            m_droppedBytes = payloadSize;

            std::cout << "FORCED_MIDDLE_NONCRITICAL_DROP"
                      << " time=" << Simulator::Now().GetSeconds()
                      << " seq=" << tcp.GetSequenceNumber()
                      << " bytes=" << payloadSize
                      << std::endl;

            return true;
        }

        return false;
    }

    void DoReset() override
    {
        m_nonCriticalSegmentsSeen = 0;
        m_dropped = false;
        m_droppedBytes = 0;
    }

    uint32_t m_nonCriticalSegmentsSeen{0};
    bool m_dropped{false};
    uint32_t m_droppedBytes{0};
};

NS_OBJECT_ENSURE_REGISTERED(DropMiddleNonCriticalSegment);

static Ptr<Socket> g_serverSocket;
static std::vector<Ptr<Socket>> g_acceptedSockets;

static std::vector<uint8_t> g_receivedData;

static uint32_t g_skipControls = 0;
static uint32_t g_criticalRetransmissions = 0;
static uint32_t g_nonCriticalRetransmissions = 0;

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

    if (skipControl)
    {
        ++g_skipControls;

        std::cout << "SKIP_TX"
                  << " time=" << Simulator::Now().GetSeconds()
                  << " seq=" << header.GetSequenceNumber()
                  << " bytes=" << header.GetUrgentPointer()
                  << std::endl;
    }
}

static void
RetransmissionTrace(Ptr<const Packet> packet,
                    const TcpHeader& header,
                    const Address& local,
                    const Address& peer,
                    Ptr<const TcpSocketBase> socket)
{
    const bool critical =
        (header.GetFlags() & TcpHeader::URG) != 0;

    if (critical)
    {
        ++g_criticalRetransmissions;
    }
    else
    {
        ++g_nonCriticalRetransmissions;
    }

    std::cout << "DATA_RETX"
              << " seq=" << header.GetSequenceNumber()
              << " bytes=" << packet->GetSize()
              << " critical=" << critical
              << std::endl;
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

        std::vector<uint8_t> bytes(size);

        const uint32_t copied =
            packet->CopyData(bytes.data(), size);

        NS_ABORT_MSG_IF(copied != size,
                        "Failed to copy application payload");

        g_receivedData.insert(g_receivedData.end(),
                              bytes.begin(),
                              bytes.end());

        std::cout << "APP_RX"
                  << " chunk=" << size
                  << " total=" << g_receivedData.size()
                  << std::endl;
    }
}

static bool
AcceptRequest(Ptr<Socket> socket, const Address& from)
{
    return true;
}

static void
AcceptConnection(Ptr<Socket> socket, const Address& from)
{
    g_acceptedSockets.push_back(socket);
    socket->SetRecvCallback(MakeCallback(&ReceiveData));
}

static int
SendFilled(Ptr<Socket> socket,
           uint32_t size,
           uint8_t marker,
           uint32_t criticalFlag)
{
    std::vector<uint8_t> payload(size, marker);

    Ptr<Packet> packet =
        Create<Packet>(payload.data(),
                       static_cast<uint32_t>(payload.size()));

    return socket->Send(packet, criticalFlag);
}

static void
ConnectionSucceeded(Ptr<Socket> socket)
{
    Ptr<TcpSocketBase> tcp =
        DynamicCast<TcpSocketBase>(socket);

    NS_ABORT_MSG_IF(tcp == nullptr,
                    "Client socket is not TcpSocketBase");

    tcp->SetSelectiveRecoveryEnabled(true);

    int result;

    result = SendFilled(socket, 100, 'A', 1);
    std::cout << "APP_TX marker=A bytes=100 critical=1 return="
              << result << std::endl;

    result = SendFilled(socket, 3000, 'B', 0);
    std::cout << "APP_TX marker=B bytes=3000 critical=0 return="
              << result << std::endl;

    result = SendFilled(socket, 100, 'C', 1);
    std::cout << "APP_TX marker=C bytes=100 critical=1 return="
              << result << std::endl;
}

static void
ConnectionFailed(Ptr<Socket> socket)
{
    NS_FATAL_ERROR("TCP connection failed");
}

int
main()
{
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;

    p2p.SetDeviceAttribute("DataRate",
                           StringValue("10Mbps"));

    p2p.SetChannelAttribute("Delay",
                            StringValue("10ms"));

    NetDeviceContainer devices =
        p2p.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper ipv4;

    ipv4.SetBase("10.1.1.0",
                 "255.255.255.0");

    Ipv4InterfaceContainer interfaces =
        ipv4.Assign(devices);

    Ptr<DropMiddleNonCriticalSegment> errorModel =
        CreateObject<DropMiddleNonCriticalSegment>();

    Ptr<PointToPointNetDevice> receiverDevice =
        DynamicCast<PointToPointNetDevice>(
            devices.Get(1));

    NS_ABORT_MSG_IF(receiverDevice == nullptr,
                    "Receiver device cast failed");

    receiverDevice->SetReceiveErrorModel(errorModel);

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

    NS_ABORT_MSG_IF(tcpClient == nullptr,
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

    Simulator::Stop(Seconds(12.0));

    Simulator::Run();
    Simulator::Destroy();

    uint32_t countA = 0;
    uint32_t countB = 0;
    uint32_t countC = 0;
    uint32_t unexpected = 0;

    for (uint8_t byte : g_receivedData)
    {
        if (byte == 'A')
        {
            ++countA;
        }
        else if (byte == 'B')
        {
            ++countB;
        }
        else if (byte == 'C')
        {
            ++countC;
        }
        else
        {
            ++unexpected;
        }
    }

    const bool transportSkipPass =
        errorModel->WasDropped() &&
        g_skipControls >= 1 &&
        g_nonCriticalRetransmissions == 0;

    // Whole-message semantics would require all 3000 B bytes to
    // disappear once any fragment of B is abandoned.
    const bool messageAtomicityPass =
        countA == 100 &&
        countB == 0 &&
        countC == 100 &&
        unexpected == 0;

    const bool partialMessageObserved =
        countB > 0 &&
        countB < 3000;

    std::cout << "SUMMARY"
              << " received=" << g_receivedData.size()
              << " A=" << countA
              << " B=" << countB
              << " C=" << countC
              << " unexpected=" << unexpected
              << " dropped_bytes=" << errorModel->DroppedBytes()
              << " skips=" << g_skipControls
              << " critical_retx=" << g_criticalRetransmissions
              << " noncritical_retx=" << g_nonCriticalRetransmissions
              << std::endl;

    std::cout << "BYTE_RANGE_SKIP="
              << (transportSkipPass ? "PASS" : "FAIL")
              << std::endl;

    std::cout << "MESSAGE_ATOMICITY="
              << (messageAtomicityPass ? "PASS" : "FAIL")
              << std::endl;

    std::cout << "PARTIAL_NONCRITICAL_MESSAGE_OBSERVED="
              << (partialMessageObserved ? "YES" : "NO")
              << std::endl;

    // This is a characterization test. Success means the current
    // semantics were measured successfully, regardless of whether
    // whole-message atomicity is currently provided.
    const bool characterizationPass =
        transportSkipPass &&
        countA == 100 &&
        countC == 100 &&
        unexpected == 0;

    std::cout << "PHASE10_CHARACTERIZATION="
              << (characterizationPass ? "PASS" : "FAIL")
              << std::endl;

    return characterizationPass ? 0 : 1;
}
