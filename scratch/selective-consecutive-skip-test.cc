#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/ppp-header.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/tcp-socket-base.h"

#include <cstdint>
#include <iostream>
#include <vector>

using namespace ns3;

class ConsecutiveLossErrorModel : public ErrorModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::ConsecutiveLossErrorModel")
                .SetParent<ErrorModel>()
                .AddConstructor<ConsecutiveLossErrorModel>();

        return tid;
    }

    void SetDropFirstSkip(bool enabled)
    {
        m_dropFirstSkip = enabled;
    }

    bool DataWasDropped() const
    {
        return m_dataDropped;
    }

    bool SkipWasDropped() const
    {
        return m_skipDropped;
    }

  private:
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

        const bool skipControl =
            payloadSize == 0 &&
            (tcp.GetFlags() & (TcpHeader::ACK | TcpHeader::URG)) ==
                (TcpHeader::ACK | TcpHeader::URG) &&
            tcp.GetUrgentPointer() > 0;

        // A and B have identical non-critical importance and are expected
        // to occupy the contiguous TCP range beginning at sequence 1.
        if (!m_dataDropped &&
            payloadSize > 0 &&
            tcp.GetSequenceNumber() == SequenceNumber32(1))
        {
            m_dataDropped = true;

            std::cout << "FORCED_DATA_DROP"
                      << " time=" << Simulator::Now().GetSeconds()
                      << " seq=" << tcp.GetSequenceNumber()
                      << " bytes=" << payloadSize
                      << std::endl;

            return true;
        }

        if (m_dropFirstSkip &&
            !m_skipDropped &&
            skipControl)
        {
            m_skipDropped = true;

            std::cout << "FORCED_SKIP_DROP"
                      << " time=" << Simulator::Now().GetSeconds()
                      << " seq=" << tcp.GetSequenceNumber()
                      << " bytes=" << tcp.GetUrgentPointer()
                      << std::endl;

            return true;
        }

        return false;
    }

    void DoReset() override
    {
        m_dataDropped = false;
        m_skipDropped = false;
    }

    bool m_dropFirstSkip{false};
    bool m_dataDropped{false};
    bool m_skipDropped{false};
};

NS_OBJECT_ENSURE_REGISTERED(ConsecutiveLossErrorModel);

static Ptr<Socket> g_serverSocket;
static std::vector<Ptr<Socket>> g_acceptedSockets;

static std::vector<uint8_t> g_receivedData;

static uint32_t g_skipControls = 0;
static uint32_t g_dataRetransmissions = 0;
static uint32_t g_criticalRetransmissions = 0;
static uint32_t g_nonCriticalRetransmissions = 0;

static bool g_skipCoversFirstRange = false;
static bool g_skipCoversSecondRange = false;

static bool g_dropFirstSkip = false;

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

        const uint32_t skipStart =
            header.GetSequenceNumber().GetValue();

        const uint32_t skipEnd =
            skipStart + header.GetUrgentPointer();

        if (skipStart <= 1 && skipEnd >= 101)
        {
            g_skipCoversFirstRange = true;
        }

        if (skipStart <= 101 && skipEnd >= 201)
        {
            g_skipCoversSecondRange = true;
        }

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
    ++g_dataRetransmissions;

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
              << " critical="
              << ((header.GetFlags() & TcpHeader::URG) != 0)
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
                        "Failed to copy received payload");

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

static void
ConnectionSucceeded(Ptr<Socket> socket)
{
    Ptr<TcpSocketBase> tcp =
        DynamicCast<TcpSocketBase>(socket);

    NS_ABORT_MSG_IF(tcp == nullptr,
                    "Client socket is not TcpSocketBase");

    tcp->SetSelectiveRecoveryEnabled(true);

    const uint32_t flags[] = {
        0, // A: non-critical, dropped
        0, // B: non-critical, dropped
        1, // C: critical
        1, // D: critical
        0  // E: non-critical
    };

    const uint8_t markers[] = {
        'A',
        'B',
        'C',
        'D',
        'E'
    };

    for (uint32_t i = 0; i < 5; ++i)
    {
        std::vector<uint8_t> payload(100, markers[i]);

        Ptr<Packet> packet =
            Create<Packet>(
                payload.data(),
                static_cast<uint32_t>(payload.size()));

        const int result =
            socket->Send(packet, flags[i]);

        std::cout << "APP_TX"
                  << " marker="
                  << static_cast<char>(markers[i])
                  << " critical=" << flags[i]
                  << " return=" << result
                  << std::endl;
    }
}

static void
ConnectionFailed(Ptr<Socket> socket)
{
    NS_FATAL_ERROR("TCP connection failed");
}

static bool
CheckReceivedContent()
{
    std::vector<uint8_t> expected;

    expected.insert(expected.end(), 100, 'C');
    expected.insert(expected.end(), 100, 'D');
    expected.insert(expected.end(), 100, 'E');

    if (g_receivedData != expected)
    {
        std::cout << "CONTENT_MISMATCH";

        for (uint32_t i = 0; i < g_receivedData.size(); ++i)
        {
            if (i >= expected.size() ||
                g_receivedData[i] != expected[i])
            {
                std::cout << " first_bad_offset=" << i;

                if (i < g_receivedData.size())
                {
                    std::cout << " actual="
                              << static_cast<char>(g_receivedData[i]);
                }

                if (i < expected.size())
                {
                    std::cout << " expected="
                              << static_cast<char>(expected[i]);
                }

                break;
            }
        }

        std::cout << std::endl;
        return false;
    }

    return true;
}

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);

    cmd.AddValue("dropFirstSkip",
                 "Also drop the first SKIP control",
                 g_dropFirstSkip);

    cmd.Parse(argc, argv);

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

    Ptr<ConsecutiveLossErrorModel> errorModel =
        CreateObject<ConsecutiveLossErrorModel>();

    errorModel->SetDropFirstSkip(g_dropFirstSkip);

    Ptr<PointToPointNetDevice> receiverDevice =
        DynamicCast<PointToPointNetDevice>(
            devices.Get(1));

    NS_ABORT_MSG_IF(
        receiverDevice == nullptr,
        "Receiver device is not PointToPointNetDevice");

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

    NS_ABORT_MSG_IF(
        tcpClient == nullptr,
        "Client socket is not TcpSocketBase");

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

    const bool contentPass =
        CheckReceivedContent();

    const bool rangeCoveragePass =
        g_skipCoversFirstRange &&
        g_skipCoversSecondRange;

    const bool faultInjectionPass =
        errorModel->DataWasDropped() &&
        (!g_dropFirstSkip || errorModel->SkipWasDropped());

    const bool pass =
        contentPass &&
        rangeCoveragePass &&
        faultInjectionPass &&
        g_receivedData.size() == 300 &&
        g_nonCriticalRetransmissions == 0;

    std::cout << "SUMMARY"
              << " drop_first_skip=" << g_dropFirstSkip
              << " received=" << g_receivedData.size()
              << " forced_data_drop="
              << (errorModel->DataWasDropped() ? "PASS" : "FAIL")
              << " forced_skip_drop="
              << (errorModel->SkipWasDropped() ? "PASS" : "N/A")
              << " skips=" << g_skipControls
              << " first_range="
              << (g_skipCoversFirstRange ? "PASS" : "FAIL")
              << " second_range="
              << (g_skipCoversSecondRange ? "PASS" : "FAIL")
              << " data_retx=" << g_dataRetransmissions
              << " critical_retx=" << g_criticalRetransmissions
              << " noncritical_retx=" << g_nonCriticalRetransmissions
              << " content="
              << (contentPass ? "PASS" : "FAIL")
              << std::endl;

    std::cout << "SELECTIVE_CONSECUTIVE_SKIP_TEST="
              << (pass ? "PASS" : "FAIL")
              << std::endl;

    return pass ? 0 : 1;
}
