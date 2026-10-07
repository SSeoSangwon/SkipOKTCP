#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <cstdint>
#include <iomanip>
#include <iostream>

using namespace ns3;

static uint64_t g_receivedBytes = 0;
static Time g_lastRxTime;

static void
SinkRx(Ptr<const Packet> packet, const Address& from)
{
    g_receivedBytes += packet->GetSize();
    g_lastRxTime = Simulator::Now();
}

int
main(int argc, char* argv[])
{
    uint64_t totalBytes = 8000000;
    uint32_t sendSize = 4096;

    CommandLine cmd(__FILE__);

    cmd.AddValue(
        "bytes",
        "Total application bytes",
        totalBytes);

    cmd.AddValue(
        "sendSize",
        "BulkSend application send size",
        sendSize);

    cmd.Parse(argc, argv);

    NodeContainer nodes;
    nodes.Create(2);

    PointToPointHelper p2p;

    p2p.SetDeviceAttribute(
        "DataRate",
        StringValue("100Mbps"));

    p2p.SetChannelAttribute(
        "Delay",
        StringValue("1ms"));

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

    const uint16_t port = 5000;

    PacketSinkHelper sinkHelper(
        "ns3::TcpSocketFactory",
        InetSocketAddress(
            Ipv4Address::GetAny(),
            port));

    ApplicationContainer sinkApps =
        sinkHelper.Install(nodes.Get(1));

    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(30.0));

    Ptr<PacketSink> sink =
        DynamicCast<PacketSink>(
            sinkApps.Get(0));

    NS_ABORT_MSG_IF(
        sink == nullptr,
        "PacketSink cast failed");

    sink->TraceConnectWithoutContext(
        "Rx",
        MakeCallback(&SinkRx));

    BulkSendHelper sender(
        "ns3::TcpSocketFactory",
        InetSocketAddress(
            interfaces.GetAddress(1),
            port));

    sender.SetAttribute(
        "MaxBytes",
        UintegerValue(totalBytes));

    sender.SetAttribute(
        "SendSize",
        UintegerValue(sendSize));

    ApplicationContainer sourceApps =
        sender.Install(nodes.Get(0));

    const Time sendStart =
        Seconds(0.1);

    sourceApps.Start(sendStart);
    sourceApps.Stop(Seconds(30.0));

    Simulator::Stop(Seconds(30.0));

    Simulator::Run();
    Simulator::Destroy();

    const bool pass =
        g_receivedBytes == totalBytes;

    double completionMs = -1.0;

    if (g_receivedBytes > 0)
    {
        completionMs =
            (g_lastRxTime - sendStart)
                .GetSeconds() *
            1000.0;
    }

    std::cout
        << std::fixed
        << std::setprecision(6)
        << "RESULT"
        << " sent=" << totalBytes
        << " received=" << g_receivedBytes
        << " completion_ms=" << completionMs
        << " valid=" << (pass ? 1 : 0)
        << std::endl;

    return pass ? 0 : 1;
}
