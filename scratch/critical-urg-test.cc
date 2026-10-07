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

static uint32_t g_normalPackets = 0;
static uint32_t g_criticalPackets = 0;

/**
 * Trace TCP data packets transmitted by the client.
 *
 * The experimental semantics are:
 *   URG = 0 -> non-critical data
 *   URG = 1 -> critical data
 */
static void
TxTrace(Ptr<const Packet> packet,
        const TcpHeader& header,
        Ptr<const TcpSocketBase> socket)
{
    if (packet->GetSize() == 0)
    {
        return;
    }

    const bool critical = (header.GetFlags() & TcpHeader::URG) != 0;

    if (critical)
    {
        ++g_criticalPackets;
    }
    else
    {
        ++g_normalPackets;
    }

    std::cout << "DATA_TX"
              << " seq=" << header.GetSequenceNumber()
              << " bytes=" << packet->GetSize()
              << " flags=" << static_cast<uint32_t>(header.GetFlags())
              << " critical=" << (critical ? 1 : 0)
              << std::endl;
}

/**
 * Drain received TCP data so that the receive buffer does not affect
 * the experiment.
 */
static void
ReceiveData(Ptr<Socket> socket)
{
    while (Ptr<Packet> packet = socket->Recv())
    {
        if (packet->GetSize() == 0)
        {
            break;
        }

        std::cout << "APP_RX"
                  << " bytes=" << packet->GetSize()
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

/**
 * Send three application writes with alternating importance:
 *
 *   100 bytes non-critical
 *   100 bytes critical
 *   100 bytes non-critical
 */
static void
ConnectionSucceeded(Ptr<Socket> socket)
{
    std::cout << "CONNECTED" << std::endl;

    int ret;

    ret = socket->Send(Create<Packet>(100), 0);
    std::cout << "APP_TX bytes=100 critical=0 return=" << ret << std::endl;

    ret = socket->Send(Create<Packet>(100), 1);
    std::cout << "APP_TX bytes=100 critical=1 return=" << ret << std::endl;

    ret = socket->Send(Create<Packet>(100), 0);
    std::cout << "APP_TX bytes=100 critical=0 return=" << ret << std::endl;
}

static void
ConnectionFailed(Ptr<Socket> socket)
{
    NS_FATAL_ERROR("TCP connection failed");
}

int
main(int argc, char* argv[])
{
    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper pointToPoint;
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    pointToPoint.SetChannelAttribute("Delay", StringValue("1ms"));

    NetDeviceContainer devices = pointToPoint.Install(nodes);

    InternetStackHelper internet;
    internet.Install(nodes);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");

    Ipv4InterfaceContainer interfaces = ipv4.Assign(devices);

    const uint16_t port = 5000;

    g_serverSocket =
        Socket::CreateSocket(nodes.Get(1), TcpSocketFactory::GetTypeId());

    InetSocketAddress serverAddress =
        InetSocketAddress(Ipv4Address::GetAny(), port);

    NS_ABORT_MSG_IF(g_serverSocket->Bind(serverAddress) != 0,
                    "Server bind failed");

    NS_ABORT_MSG_IF(g_serverSocket->Listen() != 0,
                    "Server listen failed");

    g_serverSocket->SetAcceptCallback(
        MakeCallback(&AcceptRequest),
        MakeCallback(&AcceptConnection));

    Ptr<Socket> clientSocket =
        Socket::CreateSocket(nodes.Get(0), TcpSocketFactory::GetTypeId());

    Ptr<TcpSocketBase> tcpClient =
        DynamicCast<TcpSocketBase>(clientSocket);

    NS_ABORT_MSG_IF(!tcpClient,
                    "Client socket is not TcpSocketBase");

    bool traceConnected =
        tcpClient->TraceConnectWithoutContext("Tx",
                                              MakeCallback(&TxTrace));

    NS_ABORT_MSG_IF(!traceConnected,
                    "Failed to connect TCP Tx trace");

    clientSocket->SetConnectCallback(
        MakeCallback(&ConnectionSucceeded),
        MakeCallback(&ConnectionFailed));

    InetSocketAddress destination =
        InetSocketAddress(interfaces.GetAddress(1), port);

    Simulator::Schedule(Seconds(0.1),
                        &Socket::Connect,
                        clientSocket,
                        destination);

    Simulator::Stop(Seconds(1.0));
    Simulator::Run();

    std::cout << "SUMMARY"
              << " normal=" << g_normalPackets
              << " critical=" << g_criticalPackets
              << std::endl;

    Simulator::Destroy();

    if (g_normalPackets != 2 || g_criticalPackets != 1)
    {
        std::cerr << "CRITICAL_URG_TEST=FAIL" << std::endl;
        return 1;
    }

    std::cout << "CRITICAL_URG_TEST=PASS" << std::endl;
    return 0;
}
