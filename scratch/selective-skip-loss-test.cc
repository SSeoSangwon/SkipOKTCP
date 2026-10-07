#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/tcp-socket-base.h"

#include <iostream>
#include <vector>

using namespace ns3;

static Ptr<Socket> g_serverSocket;
static std::vector<Ptr<Socket>> g_acceptedSockets;

static uint32_t g_receivedBytes = 0;
static uint32_t g_skipControls = 0;
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

    if (skipControl)
    {
        ++g_skipControls;

        std::cout << "SKIP_TX time=" << Simulator::Now().GetSeconds()
                  << " seq=" << header.GetSequenceNumber()
                  << " bytes=" << header.GetUrgentPointer()
                  << " ack=" << header.GetAckNumber()
                  << std::endl;

        return;
    }

    if (packet->GetSize() > 0)
    {
        const bool critical = (flags & TcpHeader::URG) != 0;

        std::cout << "DATA_TX"
                  << " seq=" << header.GetSequenceNumber()
                  << " bytes=" << packet->GetSize()
                  << " critical=" << critical
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
        if (packet->GetSize() == 0)
        {
            break;
        }

        g_receivedBytes += packet->GetSize();

        std::cout << "APP_RX"
                  << " bytes=" << packet->GetSize()
                  << " total=" << g_receivedBytes
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
    Ptr<TcpSocketBase> tcp = DynamicCast<TcpSocketBase>(socket);

    NS_ABORT_MSG_IF(tcp == nullptr,
                    "Client socket is not TcpSocketBase");

    tcp->SetSelectiveRecoveryEnabled(true);

    int result;

    result = socket->Send(Create<Packet>(100), 0);
    std::cout << "APP_TX bytes=100 critical=0 return="
              << result << std::endl;

    result = socket->Send(Create<Packet>(100), 1);
    std::cout << "APP_TX bytes=100 critical=1 return="
              << result << std::endl;

    result = socket->Send(Create<Packet>(100), 0);
    std::cout << "APP_TX bytes=100 critical=0 return="
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
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay", StringValue("10ms"));

    NetDeviceContainer devices = p2p.Install(nodes);

    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");

    Ipv4InterfaceContainer interfaces =
        ipv4.Assign(devices);

    Ptr<ReceiveListErrorModel> errorModel =
        CreateObject<ReceiveListErrorModel>();

    // The same receive index previously verified to drop seq=1 data.
    errorModel->SetList({2, 5});

    Ptr<PointToPointNetDevice> receiverDevice =
        DynamicCast<PointToPointNetDevice>(devices.Get(1));

    NS_ABORT_MSG_IF(receiverDevice == nullptr,
                    "Receiver device is not PointToPointNetDevice");

    receiverDevice->SetReceiveErrorModel(errorModel);

    const uint16_t port = 5000;

    g_serverSocket =
        Socket::CreateSocket(nodes.Get(1),
                             TcpSocketFactory::GetTypeId());

    g_serverSocket->Bind(
        InetSocketAddress(Ipv4Address::GetAny(), port));

    g_serverSocket->Listen();

    g_serverSocket->SetAcceptCallback(
        MakeCallback(&AcceptRequest),
        MakeCallback(&AcceptConnection));

    Ptr<Socket> client =
        Socket::CreateSocket(nodes.Get(0),
                             TcpSocketFactory::GetTypeId());

    Ptr<TcpSocketBase> tcpClient =
        DynamicCast<TcpSocketBase>(client);

    NS_ABORT_MSG_IF(tcpClient == nullptr,
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
        InetSocketAddress(interfaces.GetAddress(1), port));

    Simulator::Stop(Seconds(10.0));
    Simulator::Run();
    Simulator::Destroy();

    std::cout << "SUMMARY"
              << " received=" << g_receivedBytes
              << " skip_controls=" << g_skipControls
              << " data_retx=" << g_dataRetransmissions
              << std::endl;

    const bool pass =
        g_receivedBytes == 200 &&
        g_skipControls >= 2 &&
        g_dataRetransmissions == 0;

    std::cout << "SELECTIVE_SKIP_LOSS_TEST="
              << (pass ? "PASS" : "FAIL")
              << std::endl;

    return pass ? 0 : 1;
}
