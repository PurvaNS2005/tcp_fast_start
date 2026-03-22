/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * CS301M — Computer Networks
 * Fast-Start Evaluation: Dumbbell Topology Simulation
 *
 * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
 *
 * Topology:
 *
 *   Senders ──[100Mbps,1ms]── LeftRouter ──[10Mbps,10ms]── RightRouter ──[100Mbps,1ms]── Receivers
 *
 *   - 5 long-lived elephant flows (persistent background load)
 *   - 20 short mice flows (Poisson arrivals, log-normal sizes)
 *   - DropTail bottleneck buffer = 1 BDP ≈ 25 KB
 *
 * Usage:
 *   ./waf --run "fast-start-sim --cc=fast-start --seed=1 --threshold=1.10"
 *
 * Outputs (in results/):
 *   fct_<cc>_seed<N>.csv        — per-flow FCT log
 *   queue_<cc>_seed<N>.csv      — bottleneck queue occupancy trace
 *   throughput_<cc>_seed<N>.csv — per-second throughput of long flows
 *   retransmit_<cc>_seed<N>.csv — retransmission counts
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/tcp_fast_start.h"

#include <fstream>
#include <sstream>
#include <random>
#include <cmath>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("FastStartSim");

// ─── Globals for logging ──────────────────────────────────────────────────────
struct FlowRecord
{
  uint32_t flowId;
  double   startTime;   // seconds
  double   endTime;     // seconds
  uint64_t sizeBytes;   // bytes requested
};

static std::vector<FlowRecord>         g_flowRecords;
static std::ofstream                   g_queueFile;
static std::ofstream                   g_throughputFile;
static std::map<uint32_t, uint64_t>    g_rxBytes;      // flowId → bytes received so far
static std::map<uint32_t, double>      g_flowStart;    // flowId → start time

// ─── Queue occupancy tracer ───────────────────────────────────────────────────
static void
QueueOccupancyTrace (Ptr<QueueDisc> qd)
{
  uint32_t nBytes = qd->GetNBytes ();
  uint32_t nPackets = qd->GetNPackets ();
  g_queueFile << Simulator::Now ().GetSeconds () << ","
               << nBytes << ","
               << nPackets << "\n";
  // Schedule next sample in 1 ms
  Simulator::Schedule (MilliSeconds (1), &QueueOccupancyTrace, qd);
}

// ─── Log-normal flow size generator ──────────────────────────────────────────
static double
LogNormalSample (std::mt19937 &rng, double mu, double sigma)
{
  std::normal_distribution<double> norm (mu, sigma);
  return std::exp (norm (rng));
}

// ─── Short flow completion callback ──────────────────────────────────────────
static void
FlowComplete (uint32_t flowId, uint64_t sizeBytes, double startTime,
              std::ofstream *fctFile)
{
  double endTime = Simulator::Now ().GetSeconds ();
  *fctFile << flowId << ","
            << sizeBytes << ","
            << startTime << ","
            << endTime << ","
            << (endTime - startTime) << "\n";
  NS_LOG_INFO ("Flow " << flowId << " done in "
               << (endTime - startTime) * 1000.0 << " ms");
}

// ─── Main simulation ──────────────────────────────────────────────────────────
int
main (int argc, char *argv[])
{
  // ── Command-line parameters ───────────────────────────────────────────
  std::string ccAlgorithm = "cubic";     // "cubic" | "fast-start"
  uint32_t    seed         = 1;
  double      exitThreshold = 1.10;
  double      simDuration   = 200.0;     // seconds
  uint32_t    nLongFlows    = 5;
  uint32_t    nShortSenders = 20;
  double      shortFlowRate = 10.0;      // flows/second (Poisson lambda)
  std::string resultsDir    = "results/";

  CommandLine cmd;
  cmd.AddValue ("cc",        "Congestion control: cubic or fast-start", ccAlgorithm);
  cmd.AddValue ("seed",      "Random seed",                              seed);
  cmd.AddValue ("threshold", "Fast-Start EXIT_THRESHOLD",                exitThreshold);
  cmd.AddValue ("duration",  "Simulation duration (s)",                  simDuration);
  cmd.AddValue ("results",   "Directory for output CSVs",                resultsDir);
  cmd.Parse (argc, argv);

  RngSeedManager::SetSeed (seed);
  RngSeedManager::SetRun (seed);
  std::mt19937 rng (seed);

  // ── Congestion control selection ──────────────────────────────────────
  if (ccAlgorithm == "fast-start")
    {
      Config::SetDefault ("ns3::TcpL4Protocol::SocketType",
                          TypeIdValue (TcpFastStart::GetTypeId ()));
      Config::SetDefault ("ns3::TcpFastStart::ExitThreshold",
                          DoubleValue (exitThreshold));
      NS_LOG_INFO ("Using Fast-Start with EXIT_THRESHOLD=" << exitThreshold);
    }
  else
    {
      Config::SetDefault ("ns3::TcpL4Protocol::SocketType",
                          TypeIdValue (TcpCubic::GetTypeId ()));
      NS_LOG_INFO ("Using TCP Cubic (baseline)");
    }

  // Common TCP settings
  Config::SetDefault ("ns3::TcpSocket::SndBufSize",  UintegerValue (1 << 20)); // 1 MB
  Config::SetDefault ("ns3::TcpSocket::RcvBufSize",  UintegerValue (1 << 20));
  Config::SetDefault ("ns3::TcpSocket::SegmentSize",  UintegerValue (1460));

  // ── Node creation ─────────────────────────────────────────────────────
  uint32_t totalSenders = nLongFlows + nShortSenders;
  NodeContainer leftSenders, rightReceivers;
  leftSenders.Create (totalSenders);
  rightReceivers.Create (totalSenders);

  NodeContainer routers;
  routers.Create (2); // routers[0]=left, routers[1]=right

  // ── Internet stack ────────────────────────────────────────────────────
  InternetStackHelper internet;
  internet.Install (leftSenders);
  internet.Install (rightReceivers);
  internet.Install (routers);

  // ── Point-to-point links ──────────────────────────────────────────────
  PointToPointHelper accessLink;
  accessLink.SetDeviceAttribute  ("DataRate", StringValue ("100Mbps"));
  accessLink.SetChannelAttribute ("Delay",    StringValue ("1ms"));

  PointToPointHelper bottleneck;
  bottleneck.SetDeviceAttribute  ("DataRate", StringValue ("10Mbps"));
  bottleneck.SetChannelAttribute ("Delay",    StringValue ("10ms"));

  // Install bottleneck between routers
  NetDeviceContainer bottleneckDevs = bottleneck.Install (routers.Get (0), routers.Get (1));

  // Install access links for each sender/receiver pair
  Ipv4AddressHelper ipv4;
  std::vector<Ipv4Address> senderAddrs (totalSenders), receiverAddrs (totalSenders);

  // Bottleneck subnet
  ipv4.SetBase ("10.1.0.0", "255.255.255.0");
  Ipv4InterfaceContainer bottleneckIf = ipv4.Assign (bottleneckDevs);

  for (uint32_t i = 0; i < totalSenders; ++i)
    {
      // Left side: sender[i] ↔ leftRouter
      NetDeviceContainer leftDevs = accessLink.Install (leftSenders.Get (i), routers.Get (0));
      std::ostringstream subnet;
      subnet << "10.2." << i << ".0";
      ipv4.SetBase (subnet.str ().c_str (), "255.255.255.0");
      Ipv4InterfaceContainer leftIf = ipv4.Assign (leftDevs);
      senderAddrs[i] = leftIf.GetAddress (0);

      // Right side: rightRouter ↔ receiver[i]
      NetDeviceContainer rightDevs = accessLink.Install (routers.Get (1), rightReceivers.Get (i));
      std::ostringstream rsubnet;
      rsubnet << "10.3." << i << ".0";
      ipv4.SetBase (rsubnet.str ().c_str (), "255.255.255.0");
      Ipv4InterfaceContainer rightIf = ipv4.Assign (rightDevs);
      receiverAddrs[i] = rightIf.GetAddress (1);
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // ── Bottleneck queue: DropTail, size = 1 BDP ──────────────────────────
  // BDP = 10 Mbps × 20 ms = 200,000 bits = 25,000 bytes ≈ 17 × 1460-byte packets
  TrafficControlHelper tch;
  tch.Uninstall (bottleneckDevs);
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc",
                         "MaxSize", QueueSizeValue (QueueSize ("25000B")));
  QueueDiscContainer qdiscs = tch.Install (bottleneckDevs);

  // ── Open output files ─────────────────────────────────────────────────
  std::string suffix = ccAlgorithm + "_seed" + std::to_string (seed);
  if (ccAlgorithm == "fast-start")
    suffix += "_th" + std::to_string ((int)(exitThreshold * 100));

  std::ofstream fctFile (resultsDir + "fct_" + suffix + ".csv");
  fctFile << "flow_id,size_bytes,start_time,end_time,fct\n";

  g_queueFile.open (resultsDir + "queue_" + suffix + ".csv");
  g_queueFile << "time,bytes,packets\n";

  g_throughputFile.open (resultsDir + "throughput_" + suffix + ".csv");
  g_throughputFile << "time,flow_id,throughput_mbps\n";

  // ── Long flows (elephant background traffic) ──────────────────────────
  uint16_t longFlowPort = 9000;
  for (uint32_t i = 0; i < nLongFlows; ++i)
    {
      // Sink on receiver side
      PacketSinkHelper sink ("ns3::TcpSocketFactory",
                             InetSocketAddress (Ipv4Address::GetAny (), longFlowPort + i));
      ApplicationContainer sinkApp = sink.Install (rightReceivers.Get (i));
      sinkApp.Start (Seconds (0.0));
      sinkApp.Stop  (Seconds (simDuration));

      // Bulk-send source — runs for entire simulation
      BulkSendHelper source ("ns3::TcpSocketFactory",
                             InetSocketAddress (receiverAddrs[i], longFlowPort + i));
      source.SetAttribute ("MaxBytes", UintegerValue (0)); // unlimited
      ApplicationContainer sourceApp = source.Install (leftSenders.Get (i));
      sourceApp.Start (Seconds (0.1));
      sourceApp.Stop  (Seconds (simDuration));
    }

  // ── Short flows (mice — Poisson arrivals, log-normal sizes) ───────────
  // Log-normal parameters: median 50 KB (mu = ln(50000)), sigma = 1.0
  double mu    = std::log (50000.0);
  double sigma = 1.0;

  std::exponential_distribution<double> interArrival (shortFlowRate);
  uint32_t flowId = 0;
  uint16_t shortFlowBasePort = 10000;

  double arrivalTime = 0.5; // first flow at t=0.5s (after long flows are established)
  std::uniform_int_distribution<uint32_t> senderPick (nLongFlows, totalSenders - 1);

  while (arrivalTime < simDuration - 2.0)
    {
      uint32_t senderIdx = senderPick (rng);
      uint64_t flowSize  = static_cast<uint64_t> (
          std::max (1460.0, LogNormalSample (rng, mu, sigma)));
      uint16_t port      = shortFlowBasePort + (flowId % 50000);

      // Sink
      PacketSinkHelper sink ("ns3::TcpSocketFactory",
                             InetSocketAddress (Ipv4Address::GetAny (), port));
      ApplicationContainer sinkApp = sink.Install (rightReceivers.Get (senderIdx));
      sinkApp.Start (Seconds (arrivalTime - 0.001));
      sinkApp.Stop  (Seconds (simDuration));

      // Source
      BulkSendHelper source ("ns3::TcpSocketFactory",
                             InetSocketAddress (receiverAddrs[senderIdx], port));
      source.SetAttribute ("MaxBytes", UintegerValue (flowSize));
      ApplicationContainer sourceApp = source.Install (leftSenders.Get (senderIdx));

      // Capture flow metadata for FCT computation
      FlowRecord rec;
      rec.flowId    = flowId;
      rec.startTime = arrivalTime;
      rec.sizeBytes = flowSize;
      g_flowRecords.push_back (rec);

      sourceApp.Start (Seconds (arrivalTime));
      sourceApp.Stop  (Seconds (simDuration));

      // Schedule FCT logging when source finishes
      // (Approximate: log after max(flowSize / 10Mbps * 10, 0.5) + startTime)
      double estFct = std::max (0.5, (double)flowSize / (10e6 / 8) * 5.0);
      Simulator::Schedule (Seconds (arrivalTime + estFct),
                           &FlowComplete, flowId, flowSize, arrivalTime, &fctFile);

      arrivalTime += interArrival (rng);
      ++flowId;
    }

  NS_LOG_INFO ("Scheduled " << flowId << " short flows");

  // ── Start queue trace ─────────────────────────────────────────────────
  Simulator::Schedule (Seconds (0.0), &QueueOccupancyTrace, qdiscs.Get (0));

  // ── Flow monitor for retransmit stats ─────────────────────────────────
  FlowMonitorHelper flowMonitor;
  Ptr<FlowMonitor> monitor = flowMonitor.InstallAll ();

  // ── Run ───────────────────────────────────────────────────────────────
  Simulator::Stop (Seconds (simDuration));
  NS_LOG_INFO ("Starting simulation for " << simDuration << "s ...");
  Simulator::Run ();

  // ── Collect retransmission stats ──────────────────────────────────────
  monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier =
      DynamicCast<Ipv4FlowClassifier> (flowMonitor.GetClassifier ());
  FlowMonitor::FlowStatsContainer stats = monitor->GetFlowStats ();

  std::ofstream retxFile (resultsDir + "retransmit_" + suffix + ".csv");
  retxFile << "flow_id,tx_packets,rx_packets,lost_packets,retx_packets,"
              "mean_delay_ms,throughput_mbps\n";

  uint64_t totalTx = 0, totalRetx = 0;
  for (auto &kv : stats)
    {
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (kv.first);
      (void)t; // suppress unused warning
      FlowMonitor::FlowStats &fs = kv.second;
      totalTx   += fs.txPackets;
      totalRetx += fs.lostPackets;
      double meanDelayMs = fs.rxPackets > 0
          ? (fs.delaySum.GetMilliSeconds () / (double)fs.rxPackets) : 0.0;
      double throughputMbps = fs.rxBytes > 0
          ? (fs.rxBytes * 8.0 / simDuration / 1e6) : 0.0;

      retxFile << kv.first << ","
               << fs.txPackets << ","
               << fs.rxPackets << ","
               << fs.lostPackets << ","
               << fs.timesForwarded << ","
               << meanDelayMs << ","
               << throughputMbps << "\n";
    }

  double retxRate = totalTx > 0 ? (double)totalRetx / totalTx : 0.0;
  NS_LOG_INFO ("Overall retransmission rate: " << retxRate * 100.0 << "%");

  // Write summary
  std::ofstream summary (resultsDir + "summary_" + suffix + ".txt");
  summary << "Simulation: " << ccAlgorithm << " seed=" << seed
          << " threshold=" << exitThreshold << "\n"
          << "Short flows scheduled: " << flowId << "\n"
          << "Retransmission rate: " << retxRate * 100.0 << "%\n";

  Simulator::Destroy ();

  fctFile.close ();
  g_queueFile.close ();
  g_throughputFile.close ();
  retxFile.close ();
  summary.close ();

  NS_LOG_INFO ("Simulation complete. Results written to " << resultsDir);
  return 0;
}
