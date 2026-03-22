/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * CS301M — Computer Networks
 * Fast-Start TCP Congestion Control Algorithm
 *
 * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
 *
 * Fast-Start is a targeted modification to TCP Cubic that addresses the
 * poor bandwidth utilisation during the first few RTTs of short flows.
 *
 * Modifications over standard TCP Cubic:
 *   1. Enlarged Initial Window: IW = 20 MSS (vs RFC 6928's 10 MSS)
 *   2. RTT-Gradient Early Slow-Start Exit: proactive exit based on
 *      queuing delay detection, before packet loss occurs.
 *
 * Everything else (Cubic window function, fast retransmit, fast
 * recovery) remains unchanged to preserve long-flow behaviour.
 */

#ifndef TCP_FAST_START_H
#define TCP_FAST_START_H

#include "ns3/tcp-cubic.h"
#include "ns3/traced-value.h"
#include "ns3/data-rate.h"

namespace ns3 {

/**
 * \ingroup tcp
 * \brief Fast-Start TCP Congestion Control
 *
 * Inherits from TcpCubic and overrides only the slow-start phase.
 * The Cubic congestion avoidance window function is reused unchanged.
 */
class TcpFastStart : public TcpCubic
{
public:
  /**
   * \brief Get the type ID.
   * \return the object TypeId
   */
  static TypeId GetTypeId ();

  TcpFastStart ();
  TcpFastStart (const TcpFastStart &sock);
  ~TcpFastStart () override;

  std::string GetName () const override;

  /**
   * \brief Called when a new ACK is received.
   *
   * Implements the Fast-Start slow-start logic:
   *   - Tracks RTT_min
   *   - Checks RTT-gradient exit condition
   *   - Falls back to Cubic if neither condition applies
   */
  void IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) override;

  /**
   * \brief Called on connection initialisation.
   * Sets IW = 20 MSS (Modification A).
   */
  uint32_t GetSsThresh (Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;

  /**
   * \brief Called when a packet is sent.
   * We override to enforce IW on the very first send.
   */
  void CongestionStateSet (Ptr<TcpSocketState> tcb,
                           const TcpSocketState::TcpCongState_t newState) override;

  Ptr<TcpCongestionOps> Fork () override;

private:
  /**
   * \brief Check if RTT has inflated beyond EXIT_THRESHOLD × RTT_min.
   * \param tcb  Socket state (contains latest RTT sample via m_lastRtt)
   * \return true if early exit should be triggered
   */
  bool ShouldExitSlowStart (Ptr<TcpSocketState> tcb);

  /**
   * \brief Update the running minimum RTT estimate.
   * \param rttSample  Latest RTT sample in microseconds
   */
  void UpdateRttMin (Time rttSample);

  // ── Configuration Parameters ─────────────────────────────────────────
  uint32_t m_initialWindow;       //!< IW in MSS units (default: 20)
  double   m_exitThreshold;       //!< RTT inflation ratio triggering exit (default: 1.10)

  // ── Per-connection State ──────────────────────────────────────────────
  Time     m_rttMin;              //!< Running minimum RTT observed
  bool     m_rttMinInitialised;   //!< False until first RTT sample arrives
  bool     m_inSlowStart;         //!< True while in our custom slow-start phase
};

} // namespace ns3

#endif /* TCP_FAST_START_H */