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

class DropFinalAckErrorModel : public ErrorModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("ns3::DropFinalAckErrorModel")
                .SetParent<ErrorModel>()
                .AddConstructor<DropFinalAckErrorModel>();

        return tid;
    }

    bool WasDropped() const
    {
        return m_dropCount > 0;
    }

    uint32_t GetDropCount() const
    {
        return m_dropCount;
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

        const bool pureAck =
            copy->GetSize() == 0 &&
            (tcp.GetFlags() & TcpHeader::ACK) != 0;

        const bool blockFinalAck =
            pureAck &&
            tcp.GetAckNumber() >= SequenceNumber32(501) &&
            Simulator::Now() < Seconds(2.0);

        if (blockFinalAck)
        {
            ++m_dropCount;

            std::cout << "FORCED_ACK_DROP"
                      << " time=" << Simulator::Now().GetSeconds()
                      << " ack=" << tcp.GetAckNumber()
                      << " count=" << m_dropCount
                      << std::endl;

            return true;
        }

        return false;
    }

    void DoReset() override
    {
        m_dropCount = 0;
    }

    uint32_t m_dropCount{0};
};

NS_OBJECT_ENSURE_REGISTERED(DropFinalAckErrorModel);

static Ptr<Socket> g_serverSocket;
static std::vector<Ptr<Socket>> g_acceptedSockets;

static std::vector<uint8_t> g_receivedData;

static uint32_t g_skipControls = 0;
static uint32_t g_lateSkipControls = 0;
static uint32_t g_dataRetransmissions = 0;

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

    if (Simulator::Now() > Seconds(0.5))
    {
        ++g_lateSkipControls;
    }

    std::cout << "SKIP_TX"
              << " time=" << Simulator::Now().GetSeconds()
              << " seq=" << header.GetSequenceNumber()
              << " bytes=" << header.GetUrgentPointer()
              << std::endl;
}

static void
RetransmissionTrace(Ptr<const Packet> packet,
                    const TcpHeader& header,
                    const Address& local,
                    const Address& peer,
                    Ptr<const TcpSocketBase> socket)
{
    ++g_dataRetransmissions;

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
        0,
        1,
        0,
        1,
        0
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
            Create<Packet>(payload.data(),
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

    expected.insert(expected.end(), 100, 'B');
    expected.insert(expected.end(), 100, 'C');
    expected.insert(expected.end(), 100, 'D');
    expected.insert(expected.end(), 100, 'E');

    return g_receivedData == expected;
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

    Ptr<ReceiveListErrorModel> forwardLoss =
        CreateObject<ReceiveListErrorModel>();

    // Drop only A on the forward path.
    forwardLoss->SetList({2});

    Ptr<PointToPointNetDevice> serverDevice =
        DynamicCast<PointToPointNetDevice>(
            devices.Get(1));

    NS_ABORT_MSG_IF(serverDevice == nullptr,
                    "Server device cast failed");

    serverDevice->SetReceiveErrorModel(forwardLoss);

    Ptr<DropFinalAckErrorModel> reverseLoss =
        CreateObject<DropFinalAckErrorModel>();

    Ptr<PointToPointNetDevice> clientDevice =
        DynamicCast<PointToPointNetDevice>(
            devices.Get(0));

    NS_ABORT_MSG_IF(clientDevice == nullptr,
                    "Client device cast failed");

    clientDevice->SetReceiveErrorModel(reverseLoss);

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

    const bool ackDropPass =
        reverseLoss->WasDropped();

    const bool duplicateSkipPass =
        g_lateSkipControls >= 1;

    const bool contentPass =
        CheckReceivedContent();

    const bool retransmissionPass =
        g_dataRetransmissions == 0;

    const bool pass =
        ackDropPass &&
        duplicateSkipPass &&
        contentPass &&
        retransmissionPass;

    std::cout << "SUMMARY"
              << " received=" << g_receivedData.size()
              << " skips=" << g_skipControls
              << " late_skips=" << g_lateSkipControls
              << " ack_drops=" << reverseLoss->GetDropCount()
              << " data_retx=" << g_dataRetransmissions
              << std::endl;

    std::cout << "FINAL_ACK_DROP="
              << (ackDropPass ? "PASS" : "FAIL")
              << std::endl;

    std::cout << "DUPLICATE_SKIP_RETRANSMISSION="
              << (duplicateSkipPass ? "PASS" : "FAIL")
              << std::endl;

    std::cout << "CONTENT_AFTER_DUPLICATE_SKIP="
              << (contentPass ? "PASS" : "FAIL")
              << std::endl;

    std::cout << "NO_DATA_RETRANSMISSION="
              << (retransmissionPass ? "PASS" : "FAIL")
              << std::endl;

    std::cout << "SELECTIVE_DUPLICATE_SKIP_TEST="
              << (pass ? "PASS" : "FAIL")
              << std::endl;

    return pass ? 0 : 1;
}
