#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ppp-header.h"
#include "ns3/tcp-socket-base.h"

#include <iostream>
#include <vector>

using namespace ns3;

class DropFirstCriticalDataErrorModel : public ErrorModel
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::DropFirstCriticalDataErrorModel")
                                .SetParent<ErrorModel>()
                                .AddConstructor<DropFirstCriticalDataErrorModel>();
        return tid;
    }

    bool WasDropped() const
    {
        return m_dropped;
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
        if (copy->RemoveHeader(ipv4) == 0 || ipv4.GetProtocol() != 6)
        {
            return false;
        }

        TcpHeader tcp;
        if (copy->RemoveHeader(tcp) == 0)
        {
            return false;
        }

        const bool criticalData = copy->GetSize() > 0 &&
                                  (tcp.GetFlags() & TcpHeader::URG) != 0;

        if (!criticalData)
        {
            return false;
        }

        m_dropped = true;

        std::cout << "FORCED_CRITICAL_DROP"
                  << " time=" << Simulator::Now().GetSeconds()
                  << " seq=" << tcp.GetSequenceNumber()
                  << " bytes=" << copy->GetSize()
                  << std::endl;

        return true;
    }

    void DoReset() override
    {
        m_dropped = false;
    }

    bool m_dropped{false};
};

NS_OBJECT_ENSURE_REGISTERED(DropFirstCriticalDataErrorModel);

static Ptr<Socket> g_serverSocket;
static std::vector<Ptr<Socket>> g_acceptedSockets;

static uint32_t g_criticalRetransmissions = 0;
static uint32_t g_nonCriticalRetransmissions = 0;
static uint32_t g_receivedBytes = 0;

static void
RetransmissionTrace(Ptr<const Packet> packet,
                    const TcpHeader& header,
                    const Address& local,
                    const Address& peer,
                    Ptr<const TcpSocketBase> socket)
{
    const bool critical = (header.GetFlags() & TcpHeader::URG) != 0;

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
        if (packet->GetSize() == 0)
        {
            break;
        }

        g_receivedBytes += packet->GetSize();
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
    const int first = socket->Send(Create<Packet>(100), 0);
    const int second = socket->Send(Create<Packet>(100), 1);
    const int third = socket->Send(Create<Packet>(100), 0);

    NS_ABORT_MSG_IF(first != 100 || second != 100 || third != 100,
                    "Application data was not accepted in full");
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
    Ipv4InterfaceContainer interfaces = ipv4.Assign(devices);

    Ptr<DropFirstCriticalDataErrorModel> errorModel =
        CreateObject<DropFirstCriticalDataErrorModel>();

    Ptr<PointToPointNetDevice> receiverDevice =
        DynamicCast<PointToPointNetDevice>(devices.Get(1));

    NS_ABORT_MSG_IF(receiverDevice == nullptr,
                    "Receiver device is not PointToPointNetDevice");

    receiverDevice->SetReceiveErrorModel(errorModel);

    const uint16_t port = 5000;

    g_serverSocket = Socket::CreateSocket(nodes.Get(1), TcpSocketFactory::GetTypeId());
    g_serverSocket->Bind(InetSocketAddress(Ipv4Address::GetAny(), port));
    g_serverSocket->Listen();
    g_serverSocket->SetAcceptCallback(MakeCallback(&AcceptRequest),
                                      MakeCallback(&AcceptConnection));

    Ptr<Socket> client = Socket::CreateSocket(nodes.Get(0), TcpSocketFactory::GetTypeId());
    Ptr<TcpSocketBase> tcpClient = DynamicCast<TcpSocketBase>(client);

    NS_ABORT_MSG_IF(tcpClient == nullptr,
                    "Client socket is not TcpSocketBase");

    tcpClient->TraceConnectWithoutContext("Retransmission",
                                          MakeCallback(&RetransmissionTrace));

    client->SetConnectCallback(MakeCallback(&ConnectionSucceeded),
                               MakeCallback(&ConnectionFailed));

    Simulator::Schedule(Seconds(0.1),
                        &Socket::Connect,
                        client,
                        InetSocketAddress(interfaces.GetAddress(1), port));

    Simulator::Stop(Seconds(3.0));
    Simulator::Run();
    Simulator::Destroy();

    const bool pass = errorModel->WasDropped() &&
                      g_receivedBytes == 300 &&
                      g_criticalRetransmissions >= 1 &&
                      g_nonCriticalRetransmissions == 0;

    std::cout << "SUMMARY"
              << " forced_critical_drop=" << (errorModel->WasDropped() ? "PASS" : "FAIL")
              << " received=" << g_receivedBytes
              << " critical_retx=" << g_criticalRetransmissions
              << " noncritical_retx=" << g_nonCriticalRetransmissions
              << std::endl;

    std::cout << "CRITICAL_LOSS_TEST=" << (pass ? "PASS" : "FAIL") << std::endl;

    return pass ? 0 : 1;
}
