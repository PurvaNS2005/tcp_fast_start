/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * CS301M — Computer Networks
 * AFS Simulation: Dumbbell topology, mixed mice + elephant traffic
 *
 * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
 *
 * Topology (dumbbell):
 *
 *   Left hosts (senders)             Right hosts (receivers)
 *   LH0 ─┐                       ┌─ RH0   (elephant 1)
 *   LH1 ─┤                       ├─ RH1   (elephant 2)
 *   LH2 ─┼─ LR ══[bottleneck]══ RR ─┼─ RH2   (elephant 3)
 *   LH3 ─┤                       ├─ RH3   (elephant 4)
 *   LH4 ─┘                       └─ RH4   (elephant 5)
 *   LHM ──────────────────────────── RHM   (mice source/sink)
 *
 * Access links:   100 Mbps, 1 ms one-way delay
 * Bottleneck:      10 Mbps, 10 ms one-way delay (RTT ≈ 22 ms)
 * Bottleneck buffer: 25 KB DropTail
 *
 * Traffic:
 *   5 persistent elephant flows (TCP Cubic or AFS, depending on --cc)
 *   Short (mouse) flows: Poisson arrivals (λ = 10/s), sizes log-normal
 *     (median 50 KB, σ_ln = 1.0), sent using the selected CC.
 *
 * Outputs (CSV, to --output-dir):
 *   fct_<cc>_seed<N>[_th<T>].csv      — per-flow FCT
 *   retransmit_<cc>_seed<N>[_th<T>].csv — per-flow retransmit stats
 *   queue_<cc>_seed<N>[_th<T>].csv    — bottleneck queue samples
 *   summary_<cc>_seed<N>[_th<T>].txt  — scalar summary
 *
 * Build:
 *   Copy tcp-afs.h and tcp-afs.cc to ns-3/src/internet/model/
 *   Add them to src/internet/CMakeLists.txt (model sources)
 *   Then build this script as a scratch simulation:
 *     cp afs_sim.cc ns-3/scratch/
 *     cd ns-3 && ./ns3 run "scratch/afs_sim --cc=afs --seed=1"
 *
 * Usage:
 *   --cc=cubic|afs          congestion control algorithm (default: cubic)
 *   --seed=<N>              RNG seed (default: 1)
 *   --exit-k=<float>        AFS C3 anomaly multiplier k (default: 2.0)
 *   --iw-max=<int>          AFS C1 max initial window MSS (default: 40)
 *   --mouse-threshold=<int> AFS C2 mouse/elephant boundary bytes (default: 200000)
 *   --output-dir=<path>     directory for CSV/txt output (default: ./results)
 *   --duration=<s>          simulation duration seconds (default: 200)
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/flow-monitor-module.h"

// AFS implementation (place in src/internet/model/ and register in CMakeLists)
// For scratch-directory builds, include relatively:
// #include "../src/internet/model/tcp-afs.h"
// For convenience here we assume it is registered via TypeId string.

#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <random>
#include <cmath>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("AfsSim");

// ── Global simulation parameters ──────────────────────────────────────────────

static const uint32_t N_ELEPHANTS       = 5;
static const uint32_t N_MOUSE_SENDERS   = 1;   // single Poisson source
static const double   BOTTLENECK_BPS    = 10e6; // 10 Mbps
static const double   ACCESS_BPS        = 100e6;// 100 Mbps
static const double   BN_DELAY_MS       = 10.0; // ms one-way
static const double   AC_DELAY_MS       = 1.0;  // ms one-way
static const uint32_t BN_BUFFER_BYTES   = 25000; // 25 KB DropTail
static const uint32_t MSS_BYTES         = 1460;
static const double   MOUSE_LAMBDA      = 10.0; // arrivals/sec
static const double   MOUSE_MEDIAN_KB   = 50.0; // KB
static const double   MOUSE_SIGMA_LN    = 1.0;
static const uint32_t SINK_PORT         = 9000;
static const uint32_t ELEPHANT_PORT_BASE= 8000;

// ── Per-mouse-flow tracking ────────────────────────────────────────────────────

struct FlowRecord
{
  uint32_t  flowId;
  uint32_t  sizeBytes;
  double    startTime;
  double    endTime;
  bool      completed;
  uint64_t  txBytes;
  uint64_t  rxBytes;
};

static std::vector<FlowRecord> g_mouseFlows;
static uint32_t g_nextFlowId = 1;

// ── Queue occupancy trace ──────────────────────────────────────────────────────

static std::ofstream* g_queueFile = nullptr;

void
QueueOccupancyTrace (uint32_t /*oldVal*/, uint32_t newVal)
{
  if (g_queueFile && g_queueFile->is_open ())
    {
      *g_queueFile << Simulator::Now ().GetSeconds () << ","
                   << newVal << "\n";
    }
}

// ── Mouse flow generation via Poisson process ─────────────────────────────────

static NodeContainer   g_mouseLeft;
static NodeContainer   g_mouseRight;
static Ipv4Address     g_mouseSinkAddr;
static std::string     g_cc;
static double          g_simEnd;
static uint32_t        g_seed;
static double          g_exitK;
static uint32_t        g_iwMax;
static double          g_mouseThreshold;

/**
 * Draw log-normal flow size with given median (in bytes) and σ_ln.
 * Parametrisation: µ_ln = ln(median), σ_ln as given.
 */
static double
DrawFlowSize (std::mt19937 &rng)
{
  double mu_ln = std::log (MOUSE_MEDIAN_KB * 1024.0);
  std::lognormal_distribution<double> dist (mu_ln, MOUSE_SIGMA_LN);
  double sz = dist (rng);
  // clamp: [5 KB, 500 KB] to stay in the short-flow regime
  sz = std::max (sz, 5.0 * 1024.0);
  sz = std::min (sz, 500.0 * 1024.0);
  return sz;
}

/**
 * Schedule the next mouse flow arrival using an exponential inter-arrival.
 */
static void ScheduleNextMouse (Ptr<Node> sender, Ptr<Node> receiver,
                               Ipv4Address sinkAddr, std::mt19937 rng);

static void
StartMouseFlow (Ptr<Node> sender, Ptr<Node> /*receiver*/,
                Ipv4Address sinkAddr, uint32_t sizeBytes,
                uint32_t flowId, std::mt19937 rng)
{
  double now = Simulator::Now ().GetSeconds ();

  // Record flow start
  FlowRecord rec;
  rec.flowId    = flowId;
  rec.sizeBytes = sizeBytes;
  rec.startTime = now;
  rec.endTime   = 0.0;
  rec.completed = false;
  rec.txBytes   = 0;
  rec.rxBytes   = 0;
  g_mouseFlows.push_back (rec);

  // Create a BulkSendApplication for this mouse flow
  uint16_t port = SINK_PORT + flowId;

  // Sink
  PacketSinkHelper sinkHelper ("ns3::TcpSocketFactory",
                                InetSocketAddress (Ipv4Address::GetAny (), port));
  ApplicationContainer sinkApp = sinkHelper.Install (g_mouseRight.Get (0));
  sinkApp.Start (Seconds (now));
  sinkApp.Stop  (Seconds (g_simEnd));

  // Source
  BulkSendHelper sourceHelper ("ns3::TcpSocketFactory",
                                InetSocketAddress (sinkAddr, port));
  sourceHelper.SetAttribute ("MaxBytes",    UintegerValue (sizeBytes));
  sourceHelper.SetAttribute ("SendSize",    UintegerValue (MSS_BYTES));

  ApplicationContainer sourceApp = sourceHelper.Install (sender);
  sourceApp.Start (Seconds (now));
  sourceApp.Stop  (Seconds (g_simEnd));

  NS_LOG_INFO ("Mouse flow " << flowId << " start=" << now
               << "s size=" << sizeBytes << "B");

  // Schedule next arrival
  ScheduleNextMouse (sender, g_mouseRight.Get (0), sinkAddr, rng);
}

static void
ScheduleNextMouse (Ptr<Node> sender, Ptr<Node> receiver,
                   Ipv4Address sinkAddr, std::mt19937 rng)
{
  std::exponential_distribution<double> interArrival (MOUSE_LAMBDA);
  double dt = interArrival (rng);
  double nextTime = Simulator::Now ().GetSeconds () + dt;

  if (nextTime >= g_simEnd - 0.5)
    {
      return; // no more flows in this simulation run
    }

  uint32_t sizeBytes = static_cast<uint32_t> (DrawFlowSize (rng));
  uint32_t flowId    = g_nextFlowId++;

  Simulator::Schedule (Seconds (dt),
                       &StartMouseFlow,
                       sender, receiver, sinkAddr, sizeBytes, flowId,
                       rng);
}

// ── Configure congestion control ───────────────────────────────────────────────

static void
ConfigureCc (std::string cc, double exitK, uint32_t iwMax, double mouseThresh)
{
  if (cc == "cubic")
    {
      Config::SetDefault ("ns3::TcpL4Protocol::SocketType",
                          TypeIdValue (TcpCubic::GetTypeId ()));
      // RFC 6928 IW = 10 MSS baseline
      Config::SetDefault ("ns3::TcpSocket::InitialCwnd", UintegerValue (10));
    }
  else if (cc == "afs")
    {
      // Register AFS (must be in the ns-3 build)
      TypeId tid = TypeId::LookupByName ("ns3::TcpAdaptiveFastStart");
      Config::SetDefault ("ns3::TcpL4Protocol::SocketType", TypeIdValue (tid));
      Config::SetDefault ("ns3::TcpAdaptiveFastStart::ExitK",
                          DoubleValue (exitK));
      Config::SetDefault ("ns3::TcpAdaptiveFastStart::IwMax",
                          UintegerValue (iwMax));
      Config::SetDefault ("ns3::TcpAdaptiveFastStart::MouseThreshold",
                          DoubleValue (mouseThresh));
    }
  else
    {
      NS_FATAL_ERROR ("Unknown CC: " << cc << ". Use 'cubic' or 'afs'.");
    }

  // Common TCP settings
  Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (MSS_BYTES));
  Config::SetDefault ("ns3::TcpSocket::SndBufSize",  UintegerValue (1 << 20));
  Config::SetDefault ("ns3::TcpSocket::RcvBufSize",  UintegerValue (1 << 20));
  Config::SetDefault ("ns3::TcpSocketBase::Sack",    BooleanValue (true));
}

// ── Build file name tag ────────────────────────────────────────────────────────

static std::string
FileTag (const std::string &cc, uint32_t seed, double exitK)
{
  std::ostringstream oss;
  oss << cc << "_seed" << seed;
  if (cc == "afs")
    {
      // Encode threshold: 2.0 → "k200"
      oss << "_k" << static_cast<int> (exitK * 100);
    }
  return oss.str ();
}

// ── Main ───────────────────────────────────────────────────────────────────────

int
main (int argc, char *argv[])
{
  // Defaults
  g_cc             = "cubic";
  g_seed           = 1;
  g_exitK          = 2.0;
  g_iwMax          = 40;
  g_mouseThreshold = 200000.0;
  g_simEnd         = 200.0;
  std::string outputDir = "./results";

  CommandLine cmd (__FILE__);
  cmd.AddValue ("cc",               "Congestion control: cubic|afs",    g_cc);
  cmd.AddValue ("seed",             "RNG seed",                         g_seed);
  cmd.AddValue ("exit-k",           "AFS C3 anomaly multiplier k",      g_exitK);
  cmd.AddValue ("iw-max",           "AFS C1 max IW in MSS",             g_iwMax);
  cmd.AddValue ("mouse-threshold",  "AFS C2 mouse threshold bytes",     g_mouseThreshold);
  cmd.AddValue ("duration",         "Simulation duration (s)",          g_simEnd);
  cmd.AddValue ("output-dir",       "Output directory",                 outputDir);
  cmd.Parse (argc, argv);

  // Create output directory
  std::system (("mkdir -p " + outputDir).c_str ());

  // Seed RNGs
  RngSeedManager::SetSeed  (g_seed);
  RngSeedManager::SetRun   (g_seed);
  std::mt19937 rng (g_seed * 1234567 + 9999);

  // Configure CC
  ConfigureCc (g_cc, g_exitK, g_iwMax, g_mouseThreshold);

  // ── Build topology ────────────────────────────────────────────────────────

  // Routers
  NodeContainer routers;
  routers.Create (2); // LR(0), RR(1)
  Ptr<Node> LR = routers.Get (0);
  Ptr<Node> RR = routers.Get (1);

  // Elephant hosts
  NodeContainer elephantLeft, elephantRight;
  elephantLeft.Create  (N_ELEPHANTS);
  elephantRight.Create (N_ELEPHANTS);

  // Mouse hosts
  g_mouseLeft.Create  (1);
  g_mouseRight.Create (1);

  // Internet stack
  InternetStackHelper stack;
  stack.Install (routers);
  stack.Install (elephantLeft);
  stack.Install (elephantRight);
  stack.Install (g_mouseLeft);
  stack.Install (g_mouseRight);

  // Link helpers
  PointToPointHelper accessLink;
  accessLink.SetDeviceAttribute  ("DataRate", StringValue ("100Mbps"));
  accessLink.SetChannelAttribute ("Delay",    StringValue ("1ms"));

  PointToPointHelper bottleneckLink;
  bottleneckLink.SetDeviceAttribute  ("DataRate", StringValue ("10Mbps"));
  bottleneckLink.SetChannelAttribute ("Delay",    StringValue ("10ms"));

  // Bottleneck queue (DropTail, 25 KB)
  bottleneckLink.SetQueue ("ns3::DropTailQueue",
                           "MaxSize",
                           QueueSizeValue (QueueSize (QueueSizeUnit::BYTES,
                                                      BN_BUFFER_BYTES)));

  // IP addressing
  Ipv4AddressHelper ipv4;

  // Bottleneck link LR–RR
  NetDeviceContainer bnDevs = bottleneckLink.Install (LR, RR);
  ipv4.SetBase ("10.0.0.0", "255.255.255.0");
  Ipv4InterfaceContainer bnIf = ipv4.Assign (bnDevs);

  // Elephant access links
  std::vector<Ipv4Address> elephantRightAddrs;
  for (uint32_t i = 0; i < N_ELEPHANTS; ++i)
    {
      std::ostringstream base;
      base << "10.1." << i << ".0";
      ipv4.SetBase (base.str ().c_str (), "255.255.255.0");
      NetDeviceContainer leftDevs  = accessLink.Install (elephantLeft.Get (i), LR);
      NetDeviceContainer rightDevs = accessLink.Install (RR, elephantRight.Get (i));
      Ipv4InterfaceContainer leftIf  = ipv4.Assign (leftDevs);
      ipv4.SetBase ((base.str ().substr (0, base.str ().rfind ('.') + 1)
                     + "128").c_str (), "255.255.255.128");
      Ipv4InterfaceContainer rightIf = ipv4.Assign (rightDevs);
      elephantRightAddrs.push_back (rightIf.GetAddress (1));
    }

  // Mouse access links
  ipv4.SetBase ("10.2.0.0", "255.255.255.0");
  NetDeviceContainer mLeftDevs  = accessLink.Install (g_mouseLeft.Get (0), LR);
  Ipv4InterfaceContainer mLeftIf = ipv4.Assign (mLeftDevs);

  ipv4.SetBase ("10.3.0.0", "255.255.255.0");
  NetDeviceContainer mRightDevs = accessLink.Install (RR, g_mouseRight.Get (0));
  Ipv4InterfaceContainer mRightIf = ipv4.Assign (mRightDevs);
  g_mouseSinkAddr = mRightIf.GetAddress (1);

  // Routing
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // ── Queue occupancy tracing ───────────────────────────────────────────────

  std::string tag = FileTag (g_cc, g_seed, g_exitK);
  std::string queuePath = outputDir + "/queue_" + tag + ".csv";
  g_queueFile = new std::ofstream (queuePath);
  *g_queueFile << "time_s,bytes\n";

  // Attach trace to bottleneck queue (LR→RR device queue)
  Ptr<NetDevice>  bnDev   = bnDevs.Get (0); // LR side
  Ptr<Queue<Packet>> q = bnDev->GetObject<PointToPointNetDevice> ()
                               ->GetQueue ();
  q->TraceConnectWithoutContext ("PacketsInQueue",
    MakeCallback (&QueueOccupancyTrace));

  // Sample queue every 10 ms via a periodic poll (supplemental)
  // (The trace callback fires on every enqueue/dequeue, so this gives
  //  a continuous record already.)

  // ── Elephant applications ─────────────────────────────────────────────────

  for (uint32_t i = 0; i < N_ELEPHANTS; ++i)
    {
      uint16_t port = ELEPHANT_PORT_BASE + i;

      // Sink
      PacketSinkHelper sinkH ("ns3::TcpSocketFactory",
                               InetSocketAddress (Ipv4Address::GetAny (), port));
      ApplicationContainer sinkApp = sinkH.Install (elephantRight.Get (i));
      sinkApp.Start (Seconds (0.1));
      sinkApp.Stop  (Seconds (g_simEnd));

      // Source (persistent: MaxBytes = 0 → unlimited)
      BulkSendHelper srcH ("ns3::TcpSocketFactory",
                            InetSocketAddress (elephantRightAddrs[i], port));
      srcH.SetAttribute ("MaxBytes", UintegerValue (0));
      srcH.SetAttribute ("SendSize", UintegerValue (MSS_BYTES));
      ApplicationContainer srcApp = srcH.Install (elephantLeft.Get (i));
      srcApp.Start (Seconds (0.1));
      srcApp.Stop  (Seconds (g_simEnd));
    }

  // ── Mouse flow bootstrapper ───────────────────────────────────────────────

  // First arrival at t=0.5 s (let elephants stabilise first)
  std::exponential_distribution<double> firstArrival (MOUSE_LAMBDA);
  double firstT = 0.5 + firstArrival (rng);
  uint32_t firstSize  = static_cast<uint32_t> (DrawFlowSize (rng));
  uint32_t firstFlowId = g_nextFlowId++;

  Simulator::Schedule (Seconds (firstT),
                       &StartMouseFlow,
                       g_mouseLeft.Get (0),
                       g_mouseRight.Get (0),
                       g_mouseSinkAddr,
                       firstSize,
                       firstFlowId,
                       rng);

  // ── Flow Monitor ─────────────────────────────────────────────────────────

  FlowMonitorHelper flowMonHelper;
  Ptr<FlowMonitor>  flowMon = flowMonHelper.InstallAll ();

  // ── Run ───────────────────────────────────────────────────────────────────

  Simulator::Stop (Seconds (g_simEnd));
  Simulator::Run ();

  // ── Post-processing ───────────────────────────────────────────────────────

  flowMon->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier =
    DynamicCast<Ipv4FlowClassifier> (flowMonHelper.GetClassifier ());
  FlowMonitor::FlowStatsContainer stats = flowMon->GetFlowStats ();

  // Write FCT CSV
  std::string fctPath  = outputDir + "/fct_"         + tag + ".csv";
  std::string retxPath = outputDir + "/retransmit_"  + tag + ".csv";
  std::ofstream fctFile  (fctPath);
  std::ofstream retxFile (retxPath);

  fctFile  << "flow_id,size_bytes,fct,src,dst\n";
  retxFile << "flow_id,tx_packets,retx_packets,throughput_mbps\n";

  double totalTx   = 0, totalRetx = 0;
  uint32_t nFlows  = 0;

  for (auto &kv : stats)
    {
      Ipv4FlowClassifier::FiveTuple ft = classifier->FindFlow (kv.first);
      const FlowMonitor::FlowStats &fs = kv.second;

      // Elephant flows (ports 8000–8004)
      bool isElephant = (ft.destinationPort >= ELEPHANT_PORT_BASE &&
                         ft.destinationPort <  ELEPHANT_PORT_BASE + N_ELEPHANTS);

      double fct = fs.timeLastRxPacket.GetSeconds ()
                 - fs.timeFirstTxPacket.GetSeconds ();
      double tput_mbps = (fct > 0)
                       ? (fs.rxBytes * 8.0 / fct / 1e6)
                       : 0.0;

      uint32_t flowIdOut = isElephant
                         ? (ft.destinationPort - ELEPHANT_PORT_BASE + 1)
                         : ft.destinationPort - SINK_PORT;

      if (!isElephant && fct > 0)
        {
          // Match to our flow records by port for size_bytes lookup
          uint32_t sizeOut = MSS_BYTES * fs.txPackets; // approximate
          fctFile << flowIdOut << ","
                  << fs.txBytes  << ","
                  << fct         << ","
                  << ft.sourceAddress      << ","
                  << ft.destinationAddress << "\n";
        }

      retxFile << flowIdOut          << ","
               << fs.txPackets       << ","
               << fs.lostPackets     << ","
               << tput_mbps          << "\n";

      totalTx   += fs.txPackets;
      totalRetx += fs.lostPackets;
      nFlows++;
    }

  fctFile.close ();
  retxFile.close ();

  // Queue file already written incrementally
  g_queueFile->close ();
  delete g_queueFile;
  g_queueFile = nullptr;

  // Write summary
  double retxRate = (totalTx > 0) ? (totalRetx / totalTx * 100.0) : 0.0;
  std::string summPath = outputDir + "/summary_" + tag + ".txt";
  std::ofstream summ (summPath);
  summ << "CC: " << g_cc << "\n"
       << "Seed: " << g_seed << "\n"
       << "ExitK: " << g_exitK << "\n"
       << "IwMax: " << g_iwMax << "\n"
       << "Duration: " << g_simEnd << "s\n"
       << "Total flows: " << nFlows << "\n"
       << "Total TX packets: " << static_cast<uint64_t>(totalTx) << "\n"
       << "Total lost packets: " << static_cast<uint64_t>(totalRetx) << "\n"
       << "Retransmission rate: " << retxRate << "%\n";
  summ.close ();

  std::cout << "[AFS Sim] Done. tag=" << tag
            << " retx_rate=" << retxRate << "%"
            << " n_flows=" << nFlows << "\n";

  Simulator::Destroy ();
  return 0;
}