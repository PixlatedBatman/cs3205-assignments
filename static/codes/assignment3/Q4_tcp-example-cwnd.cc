/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <fstream>
#include <iostream>
#include <map>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("TcpExample");

static void
CwndChange(Ptr<OutputStreamWrapper> stream, uint32_t oldCwnd, uint32_t newCwnd)
{
    *stream->GetStream() << Simulator::Now().GetSeconds() << "\t" << oldCwnd << "\t" << newCwnd
                         << std::endl;
}

static void
TraceCwnd(Ptr<OutputStreamWrapper> stream)
{
    Config::ConnectWithoutContext("/NodeList/*/$ns3::TcpL4Protocol/SocketList/*/CongestionWindow",
                                  MakeBoundCallback(&CwndChange, stream));
}

int
main(int argc, char* argv[])
{
    // Change these parameters for different simulations
    std::string tcp_variant = "TcpNewReno";
    std::string bandwidth = "5Mbps";
    std::string delay = "5ms";
    std::string queuesize = "1000p";
    double error_rate = 0.000001;
    double simulation_time = 10.0; // seconds

    CommandLine cmd(__FILE__);
    cmd.AddValue("tcp_variant", "TCP variant: TcpCubic, TcpReno, TcpNewReno", tcp_variant);
    cmd.AddValue("bandwidth", "Point-to-point bandwidth", bandwidth);
    cmd.AddValue("delay", "Point-to-point link delay", delay);
    cmd.AddValue("queuesize", "Queue size", queuesize);
    cmd.AddValue("error_rate", "Receive error rate on node 1 of first link", error_rate);
    cmd.AddValue("simulation_time", "Simulation time in seconds", simulation_time);
    cmd.Parse(argc, argv);

    // Select TCP variant
    if (tcp_variant == "TcpCubic")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));
    }
    else if (tcp_variant == "TcpReno")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpReno"));
    }
    else if (tcp_variant == "TcpNewReno")
    {
        Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpNewReno"));
    }
    else
    {
        std::cerr << "Invalid TCP version\n";
        return 1;
    }

    // Optional: segment size
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1460));

    // Q2 topology:
    // n0 --- r1 --- r2 --- n2
    // n1 --- r1 --- r2 --- n3

    NodeContainer n0r1;
    n0r1.Create(2);

    NodeContainer r1r2;
    r1r2.Add(n0r1.Get(1));
    r1r2.Create(1);

    NodeContainer r2n2;
    r2n2.Add(r1r2.Get(1));
    r2n2.Create(1);

    NodeContainer n1r1;
    n1r1.Add(n0r1.Get(1));
    n1r1.Create(1);

    NodeContainer r2n3;
    r2n3.Add(r1r2.Get(1));
    r2n3.Create(1);

    // Link helpers
    PointToPointHelper pointToPoint;
    pointToPoint.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    pointToPoint.SetChannelAttribute("Delay", StringValue("2ms"));
    pointToPoint.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(queuesize));

    PointToPointHelper pointToPoint2;
    pointToPoint2.SetDeviceAttribute("DataRate", StringValue("10Mbps")); // bottleneck
    pointToPoint2.SetChannelAttribute("Delay", StringValue("20ms"));
    pointToPoint2.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(queuesize));

    PointToPointHelper pointToPoint3;
    pointToPoint3.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
    pointToPoint3.SetChannelAttribute("Delay", StringValue("2ms"));
    pointToPoint3.SetQueue("ns3::DropTailQueue", "MaxSize", StringValue(queuesize));

    // Install devices
    NetDeviceContainer devices = pointToPoint.Install(n0r1);
    NetDeviceContainer devices2 = pointToPoint2.Install(r1r2);
    NetDeviceContainer devices3 = pointToPoint3.Install(r2n2);

    NetDeviceContainer devices4 = pointToPoint.Install(n1r1);
    NetDeviceContainer devices5 = pointToPoint3.Install(r2n3);

    // Error model on r1-facing device of first link
    Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
    em->SetAttribute("ErrorRate", DoubleValue(error_rate));
    devices.Get(1)->SetAttribute("ReceiveErrorModel", PointerValue(em));

    // Install Internet stack
    InternetStackHelper stack;
    stack.InstallAll();

    // Assign IP addresses
    Ipv4InterfaceContainer i_n0r1;
    Ipv4InterfaceContainer i_r1r2;
    Ipv4InterfaceContainer i_r2n2;

    Ipv4InterfaceContainer i_n1r1;
    Ipv4InterfaceContainer i_r2n3;

    Ipv4AddressHelper address;

    address.SetBase("10.1.1.0", "255.255.255.252");
    i_n0r1 = address.Assign(devices);

    address.SetBase("10.1.2.0", "255.255.255.252");
    i_r1r2 = address.Assign(devices2);

    address.SetBase("10.1.3.0", "255.255.255.252");
    i_r2n2 = address.Assign(devices3);

    address.SetBase("10.1.4.0", "255.255.255.252");
    i_n1r1 = address.Assign(devices4);

    address.SetBase("10.1.5.0", "255.255.255.252");
    i_r2n3 = address.Assign(devices5);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // Sink on n2
    uint16_t sinkPort = 8080;
    Address sinkAddress(InetSocketAddress(i_r2n2.GetAddress(1), sinkPort));

    PacketSinkHelper packetSinkHelper("ns3::TcpSocketFactory",
                                      InetSocketAddress(Ipv4Address::GetAny(), sinkPort));
    ApplicationContainer sinkApps = packetSinkHelper.Install(r2n2.Get(1));
    sinkApps.Start(Seconds(0.0));
    sinkApps.Stop(Seconds(simulation_time));

    // Sink on n3
    uint16_t sinkPort2 = 8081;

    Address sinkAddress2(InetSocketAddress(i_r2n3.GetAddress(1), sinkPort2));

    PacketSinkHelper sinkHelper2("ns3::TcpSocketFactory",
                                 InetSocketAddress(Ipv4Address::GetAny(), sinkPort2));

    ApplicationContainer sinkApps2 = sinkHelper2.Install(r2n3.Get(1));
    sinkApps2.Start(Seconds(0.0));
    sinkApps2.Stop(Seconds(simulation_time));

    // Create TCP socket on sender
    Ptr<Socket> ns3TcpSocket = Socket::CreateSocket(n0r1.Get(0), TcpSocketFactory::GetTypeId());

    // BulkSend from n0 → n2
    BulkSendHelper bulkSender("ns3::TcpSocketFactory", sinkAddress);
    bulkSender.SetAttribute("MaxBytes", UintegerValue(0));

    ApplicationContainer sourceApps = bulkSender.Install(n0r1.Get(0));
    sourceApps.Start(Seconds(1.0));
    sourceApps.Stop(Seconds(simulation_time));

    // BulkSend from n1 → n3
    BulkSendHelper source2("ns3::TcpSocketFactory", sinkAddress2);
    source2.SetAttribute("MaxBytes", UintegerValue(0));

    ApplicationContainer sourceApps2 = source2.Install(n1r1.Get(1));
    sourceApps2.Start(Seconds(1.0));
    sourceApps2.Stop(Seconds(simulation_time));

    // CWND tracing
    AsciiTraceHelper asciiTraceHelper;
    Ptr<OutputStreamWrapper> stream =
        asciiTraceHelper.CreateFileStream("tcp-" + tcp_variant + ".cwnd");
    Simulator::Schedule(Seconds(1.0001), &TraceCwnd, stream);

    // ASCII + PCAP tracing
    pointToPoint.EnableAsciiAll(asciiTraceHelper.CreateFileStream("tcp-example-link1.tr"));
    pointToPoint2.EnableAsciiAll(asciiTraceHelper.CreateFileStream("tcp-example-link2.tr"));
    pointToPoint3.EnableAsciiAll(asciiTraceHelper.CreateFileStream("tcp-example-link3.tr"));

    pointToPoint.EnablePcapAll("tcp-example-link1");
    pointToPoint2.EnablePcapAll("tcp-example-link2");
    pointToPoint3.EnablePcapAll("tcp-example-link3");

    // FlowMonitor
    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> monitor = flowHelper.InstallAll();

    Simulator::Stop(Seconds(simulation_time));
    Simulator::Run();

    // Flow stats
    monitor->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());

    std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats();

    std::cout << "\n=== Per-flow results ===\n";
    for (const auto& flow : stats)
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(flow.first);

        double timeDuration =
            flow.second.timeLastRxPacket.GetSeconds() - flow.second.timeFirstTxPacket.GetSeconds();

        double throughputMbps = 0.0;
        if (timeDuration > 0)
        {
            throughputMbps = (flow.second.rxBytes * 8.0) / (timeDuration * 1000000.0);
        }

        std::cout << "Flow ID: " << flow.first << "\n";
        std::cout << "  Source      : " << t.sourceAddress << "\n";
        std::cout << "  Destination : " << t.destinationAddress << "\n";
        std::cout << "  Tx Bytes    : " << flow.second.txBytes << "\n";
        std::cout << "  Rx Bytes    : " << flow.second.rxBytes << "\n";
        std::cout << "  Throughput  : " << throughputMbps << " Mbps\n";
        std::cout << "  Lost Packets: " << flow.second.lostPackets << "\n\n";
    }

    // Sink stats
    Ptr<PacketSink> sink = DynamicCast<PacketSink>(sinkApps.Get(0));
    std::cout << "Total bytes received at sink: " << sink->GetTotalRx() << "\n";

    Ptr<PacketSink> sink2 = DynamicCast<PacketSink>(sinkApps2.Get(0));
    std::cout << "Total bytes received at sink2: " << sink2->GetTotalRx() << "\n";

    Simulator::Destroy();
    return 0;
}
