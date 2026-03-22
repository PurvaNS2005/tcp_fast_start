/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * CS301M — Computer Networks
 * Fast-Start TCP Congestion Control — Implementation
 *
 * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
 */

#include "tcp_fast_start.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

NS_LOG_COMPONENT_DEFINE ("TcpFastStart");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (TcpFastStart);

TypeId
TcpFastStart::GetTypeId ()
{
  static TypeId tid = TypeId ("ns3::TcpFastStart")
    .SetParent<TcpCubic> ()
    .SetGroupName ("Internet")
    .AddConstructor<TcpFastStart> ()
    .AddAttribute ("InitialWindow",
                   "Initial congestion window in MSS units (Fast-Start Modification A).",
                   UintegerValue (20),
                   MakeUintegerAccessor (&TcpFastStart::m_initialWindow),
                   MakeUintegerChecker<uint32_t> (1, 256))
    .AddAttribute ("ExitThreshold",
                   "RTT inflation ratio (RTT_current / RTT_min) that triggers "
                   "early slow-start exit (Fast-Start Modification B).",
                   DoubleValue (1.10),
                   MakeDoubleAccessor (&TcpFastStart::m_exitThreshold),
                   MakeDoubleChecker<double> (1.0, 2.0))
  ;
  return tid;
}

TcpFastStart::TcpFastStart ()
  : TcpCubic (),
    m_initialWindow (20),
    m_exitThreshold (1.10),
    m_rttMin (Time::Max ()),
    m_rttMinInitialised (false),
    m_inSlowStart (true)
{
  NS_LOG_FUNCTION (this);
}

TcpFastStart::TcpFastStart (const TcpFastStart &sock)
  : TcpCubic (sock),
    m_initialWindow (sock.m_initialWindow),
    m_exitThreshold (sock.m_exitThreshold),
    m_rttMin (sock.m_rttMin),
    m_rttMinInitialised (sock.m_rttMinInitialised),
    m_inSlowStart (sock.m_inSlowStart)
{
  NS_LOG_FUNCTION (this);
}

TcpFastStart::~TcpFastStart ()
{
  NS_LOG_FUNCTION (this);
}

std::string
TcpFastStart::GetName () const
{
  return "TcpFastStart";
}

Ptr<TcpCongestionOps>
TcpFastStart::Fork ()
{
  return CopyObject<TcpFastStart> (this);
}

// ─────────────────────────────────────────────────────────────────────────────
// Modification A: Enforce IW = 20 MSS on connection start
// ─────────────────────────────────────────────────────────────────────────────
void
TcpFastStart::CongestionStateSet (Ptr<TcpSocketState>                  tcb,
                                  const TcpSocketState::TcpCongState_t newState)
{
  NS_LOG_FUNCTION (this << tcb << newState);

  if (newState == TcpSocketState::CA_OPEN)
    {
      // Connection is initialising. Override the initial window.
      uint32_t iwBytes = m_initialWindow * tcb->m_segmentSize;
      tcb->m_initialCWnd = iwBytes;
      tcb->m_cWnd = iwBytes;
      NS_LOG_INFO ("FastStart: IW set to " << m_initialWindow
                   << " MSS = " << iwBytes << " bytes");
      m_inSlowStart = true;
    }

  // Delegate to Cubic for all other state transitions (CA_LOSS, etc.)
  TcpCubic::CongestionStateSet (tcb, newState);
}

// ─────────────────────────────────────────────────────────────────────────────
// RTT tracking helper
// ─────────────────────────────────────────────────────────────────────────────
void
TcpFastStart::UpdateRttMin (Time rttSample)
{
  if (!m_rttMinInitialised || rttSample < m_rttMin)
    {
      m_rttMin = rttSample;
      m_rttMinInitialised = true;
      NS_LOG_DEBUG ("FastStart: RTT_min updated to " << m_rttMin.GetMicroSeconds () << " µs");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Modification B: RTT-gradient early slow-start exit condition
// ─────────────────────────────────────────────────────────────────────────────
bool
TcpFastStart::ShouldExitSlowStart (Ptr<TcpSocketState> tcb)
{
  // We need at least one baseline RTT sample to compare against.
  if (!m_rttMinInitialised)
    {
      return false;
    }

  Time rttCurrent = tcb->m_lastRtt;

  // Guard: discard zero or negative RTT samples (e.g., from retransmits).
  if (rttCurrent <= Time (0))
    {
      return false;
    }

  // RTT invariant: RTT_current - RTT_min = queuing delay.
  // If RTT_current > RTT_min * EXIT_THRESHOLD, queuing is building up.
  double ratio = rttCurrent.GetDouble () / m_rttMin.GetDouble ();

  NS_LOG_DEBUG ("FastStart: RTT ratio = " << ratio
                << " (current=" << rttCurrent.GetMicroSeconds () << "µs"
                << " min=" << m_rttMin.GetMicroSeconds () << "µs"
                << " threshold=" << m_exitThreshold << ")");

  return ratio > m_exitThreshold;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main per-ACK window update
// ─────────────────────────────────────────────────────────────────────────────
void
TcpFastStart::IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked)
{
  NS_LOG_FUNCTION (this << tcb << segmentsAcked);

  // Always update RTT_min from the latest RTT sample.
  if (tcb->m_lastRtt > Time (0))
    {
      UpdateRttMin (tcb->m_lastRtt);
    }

  // ── Slow Start Phase ──────────────────────────────────────────────────
  if (m_inSlowStart && tcb->m_cWnd < tcb->m_ssThresh)
    {
      // Check Modification B: RTT-gradient early exit.
      if (ShouldExitSlowStart (tcb))
        {
          NS_LOG_INFO ("FastStart: RTT-gradient exit triggered. "
                       << "cwnd=" << tcb->m_cWnd
                       << " → ssthresh=" << tcb->m_cWnd
                       << " entering CongestionAvoidance");
          // Set ssthresh to current cwnd and move to congestion avoidance.
          tcb->m_ssThresh = tcb->m_cWnd;
          m_inSlowStart = false;

          // Hand off to Cubic's congestion avoidance for the CA update.
          TcpCubic::IncreaseWindow (tcb, segmentsAcked);
          return;
        }

      // Normal slow-start growth: +1 MSS per ACK.
      // segmentsAcked can be > 1 with delayed ACKs.
      uint32_t ssThresh = (uint32_t) tcb->m_ssThresh;
      uint32_t cWnd     = (uint32_t) tcb->m_cWnd;
      uint32_t toAdd    = std::min (segmentsAcked * tcb->m_segmentSize,
                                    ssThresh - cWnd);
      tcb->m_cWnd = std::min (cWnd + toAdd, ssThresh);

      NS_LOG_DEBUG ("FastStart slow-start: cwnd=" << tcb->m_cWnd
                    << " ssthresh=" << tcb->m_ssThresh);
      return;
    }

  // ── Congestion Avoidance Phase ────────────────────────────────────────
  // We have exited slow start (either via RTT gradient, ssthresh, or loss).
  // Delegate entirely to standard Cubic congestion avoidance.
  m_inSlowStart = false;
  TcpCubic::IncreaseWindow (tcb, segmentsAcked);
}

// ─────────────────────────────────────────────────────────────────────────────
// Loss handling — delegate fully to Cubic (states 3 & 4 untouched)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t
TcpFastStart::GetSsThresh (Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight)
{
  NS_LOG_FUNCTION (this << tcb << bytesInFlight);
  // On loss, Cubic halves cwnd → ssthresh. We reuse this exactly.
  // Mark that we are no longer in Fast-Start slow start.
  // (const_cast is needed here since GetSsThresh is logically const in ns3).
  const_cast<TcpFastStart *> (this)->m_inSlowStart = false;
  return TcpCubic::GetSsThresh (tcb, bytesInFlight);
}

} // namespace ns3