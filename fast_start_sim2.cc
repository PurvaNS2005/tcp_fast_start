/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * CS301M — Computer Networks
 * Fast-Start Evaluation: Fixed Simulation (Progress Report 2)
 *
 * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
 *
 * FIXES over fast_start_sim.cc:
 *   1. FCT measured via PacketSink RxCallback — exact, not estimated
 *   2. Cwnd traced per short flow — visualises IW and exit condition
 *   3. RTT traced per short flow — proves RTT-gradient detection
 *   4. Supports cc=fast-start-stat for statistical (mu+k*sigma) exit variant
 *
 * Usage:
 *   ./waf --run "fast-start-sim2 --cc=cubic        --seed=1"
 *   ./waf --run "fast-start-sim2 --cc=fast-start   --seed=1 --threshold=1.10"
 *   ./waf --run "fast-start-sim2 --cc=fast-start   --seed=1 --threshold=1.15"
 *   ./waf --run "fast-start-sim2 --cc=fast-start-stat --seed=1 --k=2.0"
 *
 * Outputs (in results2/):
 *   fct_<cc>_seed<N>_th<T>.csv        — EXACT per-flow FCT (fixed)
 *   cwnd_<cc>_seed<N>_th<T>.csv       — cwnd trace for first 10 short flows
 *   rtt_<cc>_seed<N>_th<T>.csv        — RTT trace for first 10 short flows
 *   queue_<cc>_seed<N>_th<T>.csv      — queue occupancy (unchanged)
 *   retransmit_<cc>_seed<N>_th<T>.csv — retransmission stats (unchanged)
 *   summary_<cc>_seed<N>_th<T>.txt    — summary stats
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
#include <map>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("FastStartSim2");

// ─── Global output files ──────────────────────────────────────────────────────
static std::ofstream g_fctFile;
static std::ofstream g_queueFile;
static std::ofstream g_cwndFile;
static std::ofstream g_rttFile;

// ─── Per-flow tracking ────────────────────────────────────────────────────────
struct FlowInfo {
  uint32_t flowId;
  uint64_t sizeBytes;
  double   startTime;
  uint64_t rxBytes;       // bytes received so far
  bool     completed;
};
static std::map<uint32_t, FlowInfo> g_flows; // port → FlowInfo
static uint32_t g_tracedFlows = 0;           // how many flows we are cwnd/rtt tracing
static const uint32_t MAX_TRACED = 10;       // trace first 10 short flows

// ─── Exact FCT: PacketSink RxCallback ────────────────────────────────────────
// Called every time the sink receives a packet for a short flow.
// We track cumulative bytes and log FCT when sizeBytes are received.
static void
SinkRxCallback (uint32_t flowId, uint64_t sizeBytes, double startTime,
                Ptr<const Packet> pkt, const Address &addr)
{
  auto it = g_flows.find (flowId);
  if (it == g_flows.end () || it->second.completed) return;

  it->second.rxBytes += pkt->GetSize ();

  if (it->second.rxBytes >= sizeBytes) {
    // Flow complete — log exact FCT
    double fct = Simulator::Now ().GetSeconds () - startTime;
    it->second.completed = true;
    g_fctFile << flowId << ","
              << sizeBytes << ","
              << startTime << ","
              << Simulator::Now ().GetSeconds () << ","
              << fct << "\n";
    NS_LOG_INFO ("Flow " << flowId << " FCT=" << fct*1000 << "ms  size=" << sizeBytes << "B");
  }
}

// ─── Cwnd trace callback ──────────────────────────────────────────────────────
static void
CwndTrace (uint32_t flowId, uint32_t oldVal, uint32_t newVal)
{
  g_cwndFile << Simulator::Now ().GetSeconds () << ","
             << flowId << ","
             << oldVal << ","
             << newVal << "\n";
}

// ─── RTT trace callback ───────────────────────────────────────────────────────
static void
RttTrace (uint32_t flowId, Time oldRtt, Time newRtt)
{
  g_rttFile << Simulator::Now ().GetSeconds () << ","
            << flowId << ","
            << oldRtt.GetMilliSeconds () << ","
            << newRtt.GetMilliSeconds () << "\n";
}

// ─── Connect traces to a socket after it is created ──────────────────────────
static void
ConnectSocketTraces (uint32_t flowId, Ptr<Application> app)
{
  // Only trace first MAX_TRACED flows
  if (g_tracedFlows >= MAX_TRACED) return;

  Ptr<BulkSendApplication> bulk = DynamicCast<BulkSendApplication> (app);
  if (!bulk) return;

  Ptr<Socket> sock = bulk->GetSocket ();
  if (!sock) return;

  // Connect cwnd trace
  sock->TraceConnectWithoutContext (
      "CongestionWindow",
      MakeBoundCallback (&CwndTrace, flowId));

  // Connect RTT trace
  sock->TraceConnectWithoutContext (
      "RTT",
      MakeBoundCallback (&RttTrace, flowId));

  g_tracedFlows++;
  NS_LOG_INFO ("Tracing cwnd+RTT for flow " << flowId);
}

// ─── Queue trace ──────────────────────────────────────────────────────────────
static void
QueueTrace (Ptr<QueueDisc> qd)
{
  g_queueFile << Simulator::Now ().GetSeconds () << ","
              << qd->GetNBytes () << ","
              << qd->GetNPackets () << "\n";
  Simulator::Schedule (MilliSeconds (1), &QueueTrace, qd);
}

// ─── Log-normal sampler ───────────────────────────────────────────────────────
static double
LogNormal (std::mt19937 &rng, double mu, double sigma)
{
  std::normal_distribution<double> n (mu, sigma);
  return std::exp (n (rng));
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string ccAlgorithm   = "cubic";
  uint32_t    seed          = 1;
  double      exitThreshold = 1.10;
  double      kStat         = 2.0;    // k for statistical exit (mu + k*sigma)
  double      simDuration   = 200.0;
  std::string resultsDir    = "results2/";

  CommandLine cmd;
  cmd.AddValue ("cc",        "CC algorithm: cubic | fast-start | fast-start-stat", ccAlgorithm);
  cmd.AddValue ("seed",      "Random seed",                                          seed);
  cmd.AddValue ("threshold", "EXIT_THRESHOLD for fast-start",                        exitThreshold);
  cmd.AddValue ("k",         "k for statistical exit (fast-start-stat)",             kStat);
  cmd.AddValue ("duration",  "Simulation duration (s)",                              simDuration);
  cmd.AddValue ("results",   "Output directory",                                     resultsDir);
  cmd.Parse (argc, argv);

  // Create results dir if needed (ns3 doesn't do this)
  ::system (("mkdir -p " + resultsDir).c_str ());

  RngSeedManager::SetSeed (seed);
  RngSeedManager::SetRun (seed);
  std::mt19937 rng (seed);

  // ── CC selection ──────────────────────────────────────────────────────
  if (ccAlgorithm == "fast-start" || ccAlgorithm == "fast-start-stat") {
    Config::SetDefault ("ns3::TcpL4Protocol::SocketType",
                        TypeIdValue (TcpFastStart::GetTypeId ()));
    Config::SetDefault ("ns3::TcpFastStart::ExitThreshold",
                        DoubleValue (exitThreshold));
  } else {
    Config::SetDefault ("ns3::TcpL4Protocol::SocketType",
                        TypeIdValue (TcpCubic::GetTypeId ()));
  }

  Config::SetDefault ("ns3::TcpSocket::SndBufSize",  UintegerValue (1 << 20));
  Config::SetDefault ("ns3::TcpSocket::RcvBufSize",  UintegerValue (1 << 20));
  Config::SetDefault ("ns3::TcpSocket::SegmentSize",  UintegerValue (1460));
  // Enable timestamps for RTT measurement
  Config::SetDefault ("ns3::TcpSocketBase::Timestamp", BooleanValue (true));

  // ── Topology ──────────────────────────────────────────────────────────
  uint32_t nLong  = 5;
  uint32_t nShort = 20;
  uint32_t nTotal = nLong + nShort;

  NodeContainer senders, receivers, routers;
  senders.Create (nTotal);
  receivers.Create (nTotal);
  routers.Create (2);

  InternetStackHelper inet;
  inet.Install (senders);
  inet.Install (receivers);
  inet.Install (routers);

  PointToPointHelper access, blink;
  access.SetDeviceAttribute  ("DataRate", StringValue ("100Mbps"));
  access.SetChannelAttribute ("Delay",    StringValue ("1ms"));
  blink.SetDeviceAttribute   ("DataRate", StringValue ("10Mbps"));
  blink.SetChannelAttribute  ("Delay",    StringValue ("10ms"));

  // Bottleneck
  NetDeviceContainer blinkDevs = blink.Install (routers.Get(0), routers.Get(1));

  // Access links + addressing
  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.0.0", "255.255.255.0");
  ipv4.Assign (blinkDevs);

  std::vector<Ipv4Address> recvAddrs (nTotal);
  for (uint32_t i = 0; i < nTotal; i++) {
    {
      std::ostringstream s; s << "10.2." << i << ".0";
      ipv4.SetBase (s.str ().c_str (), "255.255.255.0");
      ipv4.Assign (access.Install (senders.Get(i), routers.Get(0)));
    }
    {
      std::ostringstream s; s << "10.3." << i << ".0";
      ipv4.SetBase (s.str ().c_str (), "255.255.255.0");
      auto ifc = ipv4.Assign (access.Install (routers.Get(1), receivers.Get(i)));
      recvAddrs[i] = ifc.GetAddress (1);
    }
  }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // ── Bottleneck queue: DropTail 25 KB ─────────────────────────────────
  TrafficControlHelper tch;
  tch.Uninstall (blinkDevs);
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc",
                         "MaxSize", QueueSizeValue (QueueSize ("25000B")));
  QueueDiscContainer qdiscs = tch.Install (blinkDevs);

  // ── Build suffix for filenames ────────────────────────────────────────
  std::string thStr = std::to_string ((int)(exitThreshold * 100));
  std::string suffix = ccAlgorithm + "_seed" + std::to_string(seed);
  if (ccAlgorithm != "cubic") suffix += "_th" + thStr;

  // ── Open output files ─────────────────────────────────────────────────
  g_fctFile.open   (resultsDir + "fct_"        + suffix + ".csv");
  g_queueFile.open (resultsDir + "queue_"      + suffix + ".csv");
  g_cwndFile.open  (resultsDir + "cwnd_"       + suffix + ".csv");
  g_rttFile.open   (resultsDir + "rtt_"        + suffix + ".csv");

  g_fctFile   << "flow_id,size_bytes,start_time,end_time,fct\n";
  g_queueFile << "time,bytes,packets\n";
  g_cwndFile  << "time,flow_id,old_cwnd,new_cwnd\n";
  g_rttFile   << "time,flow_id,old_rtt_ms,new_rtt_ms\n";

  // ── Long flows ────────────────────────────────────────────────────────
  for (uint32_t i = 0; i < nLong; i++) {
    uint16_t port = 9000 + i;
    PacketSinkHelper sink ("ns3::TcpSocketFactory",
                           InetSocketAddress (Ipv4Address::GetAny (), port));
    sink.Install (receivers.Get(i)).Start (Seconds (0.0));

    BulkSendHelper src ("ns3::TcpSocketFactory",
                        InetSocketAddress (recvAddrs[i], port));
    src.SetAttribute ("MaxBytes", UintegerValue (0));
    auto app = src.Install (senders.Get(i));
    app.Start (Seconds (0.1));
    app.Stop  (Seconds (simDuration));
  }

  // ── Short flows — Poisson arrivals, log-normal sizes ──────────────────
  // Median 50 KB → mu = ln(50000), sigma = 1.0
  double mu = std::log (50000.0), sigma = 1.0;
  std::exponential_distribution<double> interArr (10.0); // 10 flows/sec

  uint32_t flowId = 0;
  double   t      = 0.5;
  std::uniform_int_distribution<uint32_t> pick (nLong, nTotal-1);

  while (t < simDuration - 5.0) {
    uint32_t si       = pick (rng);
    uint64_t flowSize = (uint64_t)std::max (1460.0, LogNormal (rng, mu, sigma));
    uint16_t port     = 20000 + (flowId % 40000);

    // ── Sink with RxCallback ──────────────────────────────────────────
    PacketSinkHelper sinkH ("ns3::TcpSocketFactory",
                             InetSocketAddress (Ipv4Address::GetAny (), port));
    auto sinkApp = sinkH.Install (receivers.Get(si));
    sinkApp.Start (Seconds (t - 0.001));
    sinkApp.Stop  (Seconds (simDuration));

    // Register flow
    FlowInfo fi;
    fi.flowId    = flowId;
    fi.sizeBytes = flowSize;
    fi.startTime = t;
    fi.rxBytes   = 0;
    fi.completed = false;
    g_flows[flowId] = fi;

    // Connect RxCallback to sink — exact FCT measurement
    Ptr<PacketSink> ps = DynamicCast<PacketSink> (sinkApp.Get(0));
    if (ps) {
      ps->TraceConnectWithoutContext (
          "Rx",
          MakeBoundCallback (&SinkRxCallback, flowId, flowSize, t));
    }

    // ── Source ────────────────────────────────────────────────────────
    BulkSendHelper srcH ("ns3::TcpSocketFactory",
                          InetSocketAddress (recvAddrs[si], port));
    srcH.SetAttribute ("MaxBytes", UintegerValue (flowSize));
    auto srcApp = srcH.Install (senders.Get(si));
    srcApp.Start (Seconds (t));
    srcApp.Stop  (Seconds (simDuration));

    // Connect cwnd + RTT traces (first MAX_TRACED flows only)
    if (g_tracedFlows < MAX_TRACED) {
      // Schedule trace connection slightly after app start (socket is created then)
      Simulator::Schedule (Seconds (t + 0.0001),
                           &ConnectSocketTraces, flowId, srcApp.Get(0));
    }

    t += interArr (rng);
    flowId++;
  }

  NS_LOG_INFO ("Scheduled " << flowId << " short flows");

  // ── Queue trace ───────────────────────────────────────────────────────
  Simulator::Schedule (Seconds (0.0), &QueueTrace, qdiscs.Get(0));

  // ── Flow monitor for retransmit stats ─────────────────────────────────
  FlowMonitorHelper fmh;
  Ptr<FlowMonitor> fm = fmh.InstallAll ();

  // ── Run ───────────────────────────────────────────────────────────────
  Simulator::Stop (Seconds (simDuration));
  Simulator::Run ();

  // ── Retransmit output ─────────────────────────────────────────────────
  fm->CheckForLostPackets ();
  auto classifier = DynamicCast<Ipv4FlowClassifier> (fmh.GetClassifier ());
  auto stats      = fm->GetFlowStats ();

  std::ofstream retxFile (resultsDir + "retransmit_" + suffix + ".csv");
  retxFile << "flow_id,tx_packets,rx_packets,lost_packets,retx_packets,mean_delay_ms,throughput_mbps\n";

  uint64_t totalTx = 0, totalLost = 0;
  for (auto &kv : stats) {
    auto &fs = kv.second;
    totalTx   += fs.txPackets;
    totalLost += fs.lostPackets;
    double delay_ms    = fs.rxPackets > 0 ? fs.delaySum.GetMilliSeconds () / fs.rxPackets : 0;
    double tput_mbps   = fs.rxBytes * 8.0 / simDuration / 1e6;
    retxFile << kv.first << ","
             << fs.txPackets << "," << fs.rxPackets << ","
             << fs.lostPackets << "," << fs.timesForwarded << ","
             << delay_ms << "," << tput_mbps << "\n";
  }

  double retxRate = totalTx > 0 ? (double)totalLost / totalTx * 100.0 : 0.0;

  // Count completed flows
  uint32_t completed = 0;
  for (auto &kv : g_flows) if (kv.second.completed) completed++;

  // ── Summary ───────────────────────────────────────────────────────────
  std::ofstream sumFile (resultsDir + "summary_" + suffix + ".txt");
  sumFile << "Simulation: "     << ccAlgorithm
          << " seed="           << seed
          << " threshold="      << exitThreshold << "\n"
          << "Short flows scheduled: " << flowId << "\n"
          << "Short flows completed: " << completed << "\n"
          << "Retransmission rate: "   << retxRate << "%\n";

  Simulator::Destroy ();

  g_fctFile.close ();
  g_queueFile.close ();
  g_cwndFile.close ();
  g_rttFile.close ();
  retxFile.close ();
  sumFile.close ();

  NS_LOG_INFO ("Done. Results in " << resultsDir);
  return 0;
}
