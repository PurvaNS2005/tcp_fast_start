/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * CS301M — Computer Networks
 * Adaptive Fast-Start (AFS) TCP Congestion Control — Implementation
 *
 * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
 *
 * See tcp-afs.h for full design documentation.
 *
 * ns-3 placement:
 *   src/internet/model/tcp-afs.cc   (this file)
 */

#include "tcp_afs.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

#include <cmath>
#include <numeric>
#include <algorithm>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("TcpAdaptiveFastStart");
NS_OBJECT_ENSURE_REGISTERED (TcpAdaptiveFastStart);

// ── Static bandwidth cache definition ─────────────────────────────────────────
std::unordered_map<uint32_t, double> TcpAdaptiveFastStart::s_bwCache;

// ── TypeId registration ────────────────────────────────────────────────────────

TypeId
TcpAdaptiveFastStart::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::TcpAdaptiveFastStart")
    .SetParent<TcpCubic> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpAdaptiveFastStart> ()
    // C1 parameter
    .AddAttribute ("IwMax",
                   "Hard cap on BDP-adaptive initial window (MSS units).",
                   UintegerValue (40),
                   MakeUintegerAccessor (&TcpAdaptiveFastStart::m_iwMax),
                   MakeUintegerChecker<uint32_t> (1, 100))
    // C2 parameter
    .AddAttribute ("MouseThreshold",
                   "Flow size boundary (bytes) below which a flow is classified Mouse.",
                   DoubleValue (200000.0),   // 200 KB
                   MakeDoubleAccessor (&TcpAdaptiveFastStart::m_mouseThreshold),
                   MakeDoubleChecker<double> ())
    // C3 parameters
    .AddAttribute ("ExitK",
                   "Anomaly multiplier k in RTTcur > µ + k·σ exit condition.",
                   DoubleValue (2.0),
                   MakeDoubleAccessor (&TcpAdaptiveFastStart::m_exitK),
                   MakeDoubleChecker<double> (0.5, 10.0))
    .AddAttribute ("RttWindowSize",
                   "Number of per-RTT samples in the rolling statistics window.",
                   UintegerValue (8),
                   MakeUintegerAccessor (&TcpAdaptiveFastStart::m_rttWindowSize),
                   MakeUintegerChecker<uint32_t> (2, 64))
  ;
  return tid;
}

// ── Constructor / Destructor ───────────────────────────────────────────────────

TcpAdaptiveFastStart::TcpAdaptiveFastStart ()
  : TcpCubic (),
    // C1
    m_iwMax          (40),
    m_iwSet          (false),
    // C2
    m_mouseThreshold (200000.0),
    m_rttCount       (0),
    m_bytesAckedRtt1 (0.0),
    m_bytesAckedRtt2 (0.0),
    m_totalBytesAcked(0.0),
    m_isElephant     (false),
    m_classifierFired(false),
    // C3
    m_exitK          (2.0),
    m_rttWindowSize  (8),
    m_rttMu          (0.0),
    m_rttSigma       (0.0),
    m_rttSumSq       (0.0),
    m_rttSum         (0.0),
    m_rttStatsReady  (false),
    m_lastRttBoundary(Seconds (0.0)),
    m_inAfsSlowStart (false)
{
  NS_LOG_FUNCTION (this);
}

TcpAdaptiveFastStart::TcpAdaptiveFastStart (const TcpAdaptiveFastStart &sock)
  : TcpCubic (sock),
    m_iwMax          (sock.m_iwMax),
    m_iwSet          (sock.m_iwSet),
    m_mouseThreshold (sock.m_mouseThreshold),
    m_rttCount       (sock.m_rttCount),
    m_bytesAckedRtt1 (sock.m_bytesAckedRtt1),
    m_bytesAckedRtt2 (sock.m_bytesAckedRtt2),
    m_totalBytesAcked(sock.m_totalBytesAcked),
    m_isElephant     (sock.m_isElephant),
    m_classifierFired(sock.m_classifierFired),
    m_exitK          (sock.m_exitK),
    m_rttWindowSize  (sock.m_rttWindowSize),
    m_rttSamples     (sock.m_rttSamples),
    m_rttMu          (sock.m_rttMu),
    m_rttSigma       (sock.m_rttSigma),
    m_rttSumSq       (sock.m_rttSumSq),
    m_rttSum         (sock.m_rttSum),
    m_rttStatsReady  (sock.m_rttStatsReady),
    m_lastRttBoundary(sock.m_lastRttBoundary),
    m_inAfsSlowStart (sock.m_inAfsSlowStart)
{
  NS_LOG_FUNCTION (this);
}

TcpAdaptiveFastStart::~TcpAdaptiveFastStart ()
{
  NS_LOG_FUNCTION (this);
}

std::string
TcpAdaptiveFastStart::GetName () const
{
  return "TcpAdaptiveFastStart";
}

Ptr<TcpCongestionOps>
TcpAdaptiveFastStart::Fork ()
{
  return CopyObject<TcpAdaptiveFastStart> (this);
}

// ── C1: BDP-Adaptive Initial Window ───────────────────────────────────────────

uint32_t
TcpAdaptiveFastStart::ComputeAdaptiveIw (Ptr<TcpSocketState> tcb) const
{
  // RTT₀ — from the SYN/SYN-ACK exchange (stored in tcb->m_minRtt after
  // TcpSocketBase processes the SYN-ACK).
  double rtt0_s = tcb->m_minRtt.IsZero ()
                    ? 0.022           // fallback: 22 ms from simulation params
                    : tcb->m_minRtt.GetSeconds ();

  // rwnd — receiver's advertised window size in bytes
  uint32_t rwnd = tcb->m_rxBuffer ? tcb->m_rxBuffer->MaxBufferSize () : 65536;

  // Cached bandwidth estimate for this /24 subnet.
  // In the simulation, peers are on 10.x.x.x; we derive a dummy key.
  // A real implementation would key on the actual peer /24.
  uint32_t subnetKey = 0; // simplified: single subnet in dumbbell topology
  double bwEst_bps = LookupBwCache (subnetKey);

  // BDP estimate (bytes)
  double bdpBytes = (bwEst_bps / 8.0) * rtt0_s;
  uint32_t mss = tcb->m_segmentSize;

  // IW = min(BDP/MSS,  rwnd/(2·MSS),  IwMax)
  uint32_t iw_bdp  = static_cast<uint32_t> (bdpBytes / mss);
  uint32_t iw_rwnd = rwnd / (2 * mss);
  uint32_t iw      = std::min ({iw_bdp, iw_rwnd, m_iwMax});

  // Enforce a minimum of 10 MSS (never worse than RFC 6928 baseline)
  iw = std::max (iw, static_cast<uint32_t> (10));

  NS_LOG_INFO ("AFS C1: rtt0=" << rtt0_s*1000 << "ms bw=" << bwEst_bps/1e6
               << "Mbps BDP=" << bdpBytes << "B IW=" << iw << "MSS");
  return iw;
}

void
TcpAdaptiveFastStart::UpdateBwCache (uint32_t subnetKey, double bwEstBps)
{
  s_bwCache[subnetKey] = bwEstBps;
}

double
TcpAdaptiveFastStart::LookupBwCache (uint32_t subnetKey) const
{
  auto it = s_bwCache.find (subnetKey);
  if (it != s_bwCache.end ())
    {
      return it->second;
    }
  // Default: assume bottleneck link bandwidth from simulation parameters
  // (10 Mbps). In a real system, a more conservative default like 1 Mbps
  // is safer, but for the dumbbell topology we seed with the known BW.
  return 10e6; // 10 Mbps default
}

// ── CongestionStateSet — C1 fires here ────────────────────────────────────────

void
TcpAdaptiveFastStart::CongestionStateSet (Ptr<TcpSocketState> tcb,
                                          const TcpSocketState::TcpCongState_t newState)
{
  NS_LOG_FUNCTION (this << tcb << newState);

  // Let Cubic handle its own state transitions first
  TcpCubic::CongestionStateSet (tcb, newState);

  if (newState == TcpSocketState::CA_OPEN && !m_iwSet)
    {
      // C1: Compute and set the BDP-adaptive initial congestion window
      uint32_t adaptiveIw = ComputeAdaptiveIw (tcb);
      uint32_t iwBytes    = adaptiveIw * tcb->m_segmentSize;

      tcb->m_initialCWnd  = iwBytes;
      tcb->m_cWnd         = iwBytes;

      // Initialise ssthresh high so we stay in Slow Start until C3 fires
      // (same as standard TCP: ssthresh starts at max)
      if (tcb->m_ssThresh < iwBytes)
        {
          tcb->m_ssThresh = tcb->m_initialSsThresh;
        }

      m_iwSet          = true;
      m_inAfsSlowStart = true;

      // C3 bootstrap: seed µ from RTT₀ with σ = 0 (updated on RTT 1)
      double rtt0_s = tcb->m_minRtt.IsZero () ? 0.022 : tcb->m_minRtt.GetSeconds ();
      m_rttSamples.push_back (rtt0_s);
      m_rttSum   = rtt0_s;
      m_rttSumSq = rtt0_s * rtt0_s;
      m_rttMu    = rtt0_s;
      m_rttSigma = 0.0;

      m_lastRttBoundary = Simulator::Now ();

      NS_LOG_INFO ("AFS C1: IW set to " << adaptiveIw << " MSS ("
                   << iwBytes << " bytes). AFS slow-start active.");
    }
}

// ── C3: Statistical RTT Anomaly Detector ──────────────────────────────────────

void
TcpAdaptiveFastStart::UpdateRttStats (double rttSample)
{
  // Add new sample to the sliding window
  m_rttSamples.push_back (rttSample);
  m_rttSum   += rttSample;
  m_rttSumSq += rttSample * rttSample;

  // Evict oldest sample if window is full
  if (m_rttSamples.size () > m_rttWindowSize)
    {
      double old  = m_rttSamples.front ();
      m_rttSamples.pop_front ();
      m_rttSum   -= old;
      m_rttSumSq -= old * old;
    }

  uint32_t n = static_cast<uint32_t> (m_rttSamples.size ());
  m_rttMu    = m_rttSum / n;

  // Variance: E[x²] - (E[x])²   (population variance; sufficient for our test)
  double var = (m_rttSumSq / n) - (m_rttMu * m_rttMu);
  m_rttSigma = (var > 0.0) ? std::sqrt (var) : 0.0;

  if (n >= 2)
    {
      m_rttStatsReady = true;
    }

  NS_LOG_DEBUG ("AFS C3: RTT sample=" << rttSample*1000 << "ms "
                << "µ=" << m_rttMu*1000 << "ms σ=" << m_rttSigma*1000 << "ms");
}

bool
TcpAdaptiveFastStart::ShouldExitSlowStart (double currentRtt) const
{
  if (!m_rttStatsReady)
    {
      return false; // need ≥2 samples to form a reliable test
    }

  // One-sided statistical test: reject H₀ (uncongested) at 97.7% confidence
  // (k=2 ↔ 2σ above mean under approximate normality of queuing delay)
  double threshold = m_rttMu + m_exitK * m_rttSigma;

  NS_LOG_DEBUG ("AFS C3: exit check RTTcur=" << currentRtt*1000
                << "ms threshold=" << threshold*1000 << "ms"
                << " (µ=" << m_rttMu*1000 << " k·σ=" << m_exitK*m_rttSigma*1000 << ")");

  return (currentRtt > threshold);
}

// ── C2: Online Flow Classifier ────────────────────────────────────────────────

void
TcpAdaptiveFastStart::RunFlowClassifier (Ptr<TcpSocketState> tcb)
{
  if (m_classifierFired)
    {
      return;
    }
  m_classifierFired = true;

  double B1 = m_bytesAckedRtt1;
  double B2 = m_bytesAckedRtt2;

  if (B1 <= 0.0 || B2 <= 0.0)
    {
      NS_LOG_WARN ("AFS C2: insufficient ACK data for classifier (B1=" << B1
                   << " B2=" << B2 << "). Assuming Mouse.");
      m_isElephant = false;
      return;
    }

  // Exponential growth ratio
  double g = B2 / B1;

  // Estimate remaining RTTs to ssthresh under Slow Start
  // N ≈ log2(ssthresh / cwnd)
  double cwnd = static_cast<double> (tcb->m_cWnd.Get ());
  double ssth = static_cast<double> (tcb->m_ssThresh.Get ());
  double N    = (cwnd > 0.0 && ssth > cwnd)
                  ? std::log2 (ssth / cwnd)
                  : 2.0; // default look-ahead if already near ssthresh
  N = std::max (N, 1.0);

  // Projected total transfer size
  double S_hat = B2 * std::pow (g, N);

  m_isElephant = (S_hat >= m_mouseThreshold);

  NS_LOG_INFO ("AFS C2: B1=" << B1 << "B B2=" << B2 << "B g=" << g
               << " N=" << N << " S_hat=" << S_hat/1024.0 << "KB "
               << (m_isElephant ? "→ ELEPHANT (hand off to Cubic CA)" : "→ Mouse (stay AFS)"));

  if (m_isElephant)
    {
      // Immediately exit AFS and hand control to Cubic Congestion Avoidance
      // Safety guarantee: long-flow impact limited to ≤2 RTTs of AFS startup
      tcb->m_ssThresh     = tcb->m_cWnd;
      m_inAfsSlowStart    = false;
      NS_LOG_INFO ("AFS C2: Elephant detected. ssthresh←cwnd=" << tcb->m_ssThresh
                   << ". Handing off to Cubic CA.");
    }
}

// ── IncreaseWindow — C2 + C3 run here ────────────────────────────────────────

void
TcpAdaptiveFastStart::IncreaseWindow (Ptr<TcpSocketState> tcb,
                                      uint32_t segmentsAcked)
{
  NS_LOG_FUNCTION (this << tcb << segmentsAcked);

  // If AFS slow-start is no longer active, delegate to Cubic entirely
  if (!m_inAfsSlowStart || m_isElephant)
    {
      TcpCubic::IncreaseWindow (tcb, segmentsAcked);
      return;
    }

  // Track cumulative bytes ACKed (used by C2 at RTT boundaries)
  m_totalBytesAcked += static_cast<double> (segmentsAcked) * tcb->m_segmentSize;

  // ── Detect RTT boundary ───────────────────────────────────────────────────
  // We use the simplest ns-3-compatible method: compare current simulation
  // time against the last RTT boundary time + current RTT estimate.
  // tcb->m_lastRtt gives the most recent RTT sample.
  Time now    = Simulator::Now ();
  Time rttEst = tcb->m_lastRtt.IsZero ()
                  ? MilliSeconds (22)   // fallback to simulation baseline
                  : tcb->m_lastRtt;

  bool atRttBoundary = (now >= m_lastRttBoundary + rttEst);

  if (atRttBoundary)
    {
      m_lastRttBoundary = now;
      m_rttCount++;

      // Record per-RTT RTT sample (in seconds)
      double rttSample_s = rttEst.GetSeconds ();
      UpdateRttStats (rttSample_s);

      // C2: Record B1 and B2; run classifier after RTT 2
      if (m_rttCount == 1)
        {
          m_bytesAckedRtt1 = m_totalBytesAcked;
          NS_LOG_DEBUG ("AFS C2: RTT1 complete, B1=" << m_bytesAckedRtt1 << "B");
        }
      else if (m_rttCount == 2)
        {
          m_bytesAckedRtt2 = m_totalBytesAcked;
          NS_LOG_DEBUG ("AFS C2: RTT2 complete, B2=" << m_bytesAckedRtt2 << "B");
          RunFlowClassifier (tcb);

          // If classifier handed off to Cubic, delegate immediately
          if (m_isElephant)
            {
              TcpCubic::IncreaseWindow (tcb, segmentsAcked);
              return;
            }
        }

      // C3: Statistical exit check on each RTT boundary (mouse path)
      if (m_rttCount >= 2 && !m_isElephant)
        {
          if (ShouldExitSlowStart (rttSample_s))
            {
              NS_LOG_INFO ("AFS C3: RTT anomaly detected at RTT " << m_rttCount
                           << ". Exiting slow-start. ssthresh←cwnd="
                           << tcb->m_cWnd);
              tcb->m_ssThresh  = tcb->m_cWnd;
              m_inAfsSlowStart = false;
              TcpCubic::IncreaseWindow (tcb, segmentsAcked);
              return;
            }
        }
    }

  // ── Standard Slow Start window increase ──────────────────────────────────
  // Remain in standard exponential growth (identical to Cubic's slow start)
  // while we are still below ssthresh and no exit has been triggered.
  if (tcb->m_cWnd < tcb->m_ssThresh)
    {
      // One MSS per ACKed segment (exponential doubling per RTT)
      uint32_t increase = std::min (segmentsAcked * tcb->m_segmentSize,
                                    tcb->m_ssThresh.Get () - tcb->m_cWnd.Get ());
      tcb->m_cWnd += increase;
      NS_LOG_DEBUG ("AFS slow-start: cwnd=" << tcb->m_cWnd << " ssthresh=" << tcb->m_ssThresh);
    }
  else
    {
      // Crossed ssthresh without an RTT-anomaly exit — transition to Cubic CA
      m_inAfsSlowStart = false;
      TcpCubic::IncreaseWindow (tcb, segmentsAcked);
    }
}

// ── GetSsThresh — delegate to Cubic (loss not handled by AFS) ─────────────────

uint32_t
TcpAdaptiveFastStart::GetSsThresh (Ptr<const TcpSocketState> tcb,
                                    uint32_t bytesInFlight)
{
  NS_LOG_FUNCTION (this << tcb << bytesInFlight);
  if (bytesInFlight > 0 && tcb->m_congState != TcpSocketState::CA_OPEN)
    {
      m_inAfsSlowStart = false;
    }
  return TcpCubic::GetSsThresh (tcb, bytesInFlight);
}

} // namespace ns3