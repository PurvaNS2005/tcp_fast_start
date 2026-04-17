// /* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
// /**
//  * CS301M — Computer Networks
//  * Fast-Start TCP Congestion Control Algorithm
//  *
//  * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
//  *
//  * Fast-Start is a targeted modification to TCP Cubic that addresses the
//  * poor bandwidth utilisation during the first few RTTs of short flows.
//  *
//  * Modifications over standard TCP Cubic:
//  *   1. Enlarged Initial Window: IW = 20 MSS (vs RFC 6928's 10 MSS)
//  *   2. RTT-Gradient Early Slow-Start Exit: proactive exit based on
//  *      queuing delay detection, before packet loss occurs.
//  *
//  * Everything else (Cubic window function, fast retransmit, fast
//  * recovery) remains unchanged to preserve long-flow behaviour.
//  */

// #ifndef TCP_FAST_START_H
// #define TCP_FAST_START_H

// #include "ns3/tcp-cubic.h"
// #include "ns3/traced-value.h"
// #include "ns3/data-rate.h"

// namespace ns3 {

// /**
//  * \ingroup tcp
//  * \brief Fast-Start TCP Congestion Control
//  *
//  * Inherits from TcpCubic and overrides only the slow-start phase.
//  * The Cubic congestion avoidance window function is reused unchanged.
//  */
// class TcpFastStart : public TcpCubic
// {
// public:
//   /**
//    * \brief Get the type ID.
//    * \return the object TypeId
//    */
//   static TypeId GetTypeId ();

//   TcpFastStart ();
//   TcpFastStart (const TcpFastStart &sock);
//   ~TcpFastStart () override;

//   std::string GetName () const override;

//   /**
//    * \brief Called when a new ACK is received.
//    *
//    * Implements the Fast-Start slow-start logic:
//    *   - Tracks RTT_min
//    *   - Checks RTT-gradient exit condition
//    *   - Falls back to Cubic if neither condition applies
//    */
//   void IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) override;

//   /**
//    * \brief Called on connection initialisation.
//    * Sets IW = 20 MSS (Modification A).
//    */
//   uint32_t GetSsThresh (Ptr<const TcpSocketState> tcb, uint32_t bytesInFlight) override;

//   /**
//    * \brief Called when a packet is sent.
//    * We override to enforce IW on the very first send.
//    */
//   void CongestionStateSet (Ptr<TcpSocketState> tcb,
//                            const TcpSocketState::TcpCongState_t newState) override;

//   Ptr<TcpCongestionOps> Fork () override;

// private:
//   /**
//    * \brief Check if RTT has inflated beyond EXIT_THRESHOLD × RTT_min.
//    * \param tcb  Socket state (contains latest RTT sample via m_lastRtt)
//    * \return true if early exit should be triggered
//    */
//   bool ShouldExitSlowStart (Ptr<TcpSocketState> tcb);

//   /**
//    * \brief Update the running minimum RTT estimate.
//    * \param rttSample  Latest RTT sample in microseconds
//    */
//   void UpdateRttMin (Time rttSample);

//   // ── Configuration Parameters ─────────────────────────────────────────
//   uint32_t m_initialWindow;       //!< IW in MSS units (default: 20)
//   double   m_exitThreshold;       //!< RTT inflation ratio triggering exit (default: 1.10)

//   // ── Per-connection State ──────────────────────────────────────────────
//   Time     m_rttMin;              //!< Running minimum RTT observed
//   bool     m_rttMinInitialised;   //!< False until first RTT sample arrives
//   bool     m_inSlowStart;         //!< True while in our custom slow-start phase
// };

// } // namespace ns3

// #endif /* TCP_FAST_START_H */


/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * CS301M — Computer Networks
 * Adaptive Fast-Start (AFS) TCP Congestion Control
 *
 * Authors: Anshuman Dave (231EE107) | Purva Siddapurmath (231EE242)
 *          anshuman.231ee107@nitk.edu.in | purvasiddapurmath.231ee242@nitk.edu.in
 *
 * AFS is a sender-only modification to TCP Cubic that targets the startup
 * phase of short flows. It comprises three components:
 *
 *   C1 – BDP-Adaptive Initial Window
 *        Reads RTT₀ and rwnd from the SYN-ACK and computes an
 *        optimal IW per connection, replacing the fixed RFC 6928 value.
 *
 *   C2 – Online Flow Classifier
 *        After two RTTs, fits an exponential growth model to ACK bytes
 *        and labels the flow Mouse (<200 KB) or Elephant (≥200 KB).
 *        Elephants are immediately handed off to Cubic CA.
 *
 *   C3 – Statistical RTT Anomaly Detector
 *        Maintains a rolling µ and σ of per-RTT RTT samples. Exits
 *        Slow Start when RTTcur > µ + k·σ  (default k = 2.0).
 *
 * All other Cubic behaviour (cubic window function, fast retransmit,
 * fast recovery, loss handling) is left entirely unchanged.
 *
 * ns-3 placement:
 *   src/internet/model/tcp-afs.h   (this file)
 *   src/internet/model/tcp-afs.cc
 *
 * Registration: add both files to src/internet/CMakeLists.txt.
 */

#ifndef TCP_AFS_H
#define TCP_AFS_H

#include "ns3/tcp-cubic.h"
#include "ns3/data-rate.h"
#include "ns3/traced-value.h"

#include <deque>
#include <unordered_map>

namespace ns3 {

/**
 * \ingroup tcp
 * \brief Adaptive Fast-Start TCP Congestion Control (AFS)
 *
 * Inherits TcpCubic and overrides exactly three methods:
 *   CongestionStateSet  — C1 fires on CA_OPEN, sets adaptive IW
 *   IncreaseWindow      — C2 + C3 run on every ACK during Slow Start
 *   GetSsThresh         — delegates to TcpCubic; AFS does not modify
 *                         ssthresh on loss, only on RTT-anomaly exit
 */
class TcpAdaptiveFastStart : public TcpCubic
{
public:
  /**
   * \brief Get the TypeId.
   * \return the object TypeId
   */
  static TypeId GetTypeId ();

  TcpAdaptiveFastStart ();
  TcpAdaptiveFastStart (const TcpAdaptiveFastStart &sock);
  ~TcpAdaptiveFastStart () override;

  std::string GetName () const override;

  // ── Three overridden methods ──────────────────────────────────────────────

  /**
   * C1: Called on state transition to CA_OPEN.
   * Reads RTT₀ from tcb->m_minRtt and rwnd, computes BDP estimate,
   * and sets m_initialCwnd = min(BDP/MSS, rwnd/(2·MSS), IWMAX).
   */
  void CongestionStateSet (Ptr<TcpSocketState> tcb,
                           const TcpSocketState::TcpCongState_t newState) override;

  /**
   * C2 + C3: Called on every ACK during Slow Start.
   * At RTT boundaries: updates rolling µ/σ (C3), runs classifier (C2),
   * checks statistical exit (C3). On exit: ssthresh ← cwnd, delegate to Cubic.
   */
  void IncreaseWindow (Ptr<TcpSocketState> tcb, uint32_t segmentsAcked) override;

  /**
   * Loss: delegate entirely to TcpCubic.
   * AFS does not modify ssthresh on loss.
   */
  uint32_t GetSsThresh (Ptr<const TcpSocketState> tcb,
                        uint32_t bytesInFlight) override;

  Ptr<TcpCongestionOps> Fork () override;

private:
  // ── C1: BDP-Adaptive IW ────────────────────────────────────────────────────

  /**
   * \brief Compute the BDP-adaptive initial window (Eq. 1 in report).
   * IW = min( BDPest/MSS,  rwnd/(2·MSS),  m_iwMax )
   */
  uint32_t ComputeAdaptiveIw (Ptr<TcpSocketState> tcb) const;

  /**
   * \brief Update the per-subnet bandwidth cache after a connection closes.
   * \param subnetKey  /24 subnet hash key of the peer
   * \param bwEstBps   Final bandwidth estimate in bits/s
   */
  void UpdateBwCache (uint32_t subnetKey, double bwEstBps);

  /**
   * \brief Look up bandwidth estimate for a peer.
   * Returns a default of 1 Mbps if no prior measurement exists.
   */
  double LookupBwCache (uint32_t subnetKey) const;

  // ── C2: Online Flow Classifier ─────────────────────────────────────────────

  /**
   * \brief Run the exponential-fit classifier after RTT 2.
   * Sets m_isElephant and, if true, immediately hands off to Cubic CA.
   * \param tcb  Socket state (to set ssthresh on elephant detection)
   */
  void RunFlowClassifier (Ptr<TcpSocketState> tcb);

  // ── C3: Statistical RTT Anomaly Detector ───────────────────────────────────

  /**
   * \brief Record a new per-RTT RTT sample and update µ, σ.
   * \param rttSample  RTT measured at the end of this RTT (in seconds)
   */
  void UpdateRttStats (double rttSample);

  /**
   * \brief Check the statistical exit condition (Eq. 5 in report).
   * Returns true when RTTcur > µ + k·σ
   * \param currentRtt  Latest RTT sample (seconds)
   */
  bool ShouldExitSlowStart (double currentRtt) const;

  // ── Configuration Attributes ───────────────────────────────────────────────

  uint32_t m_iwMax;           //!< Hard cap on adaptive IW in MSS (default 40)
  double   m_mouseThreshold;  //!< Mouse/elephant boundary in bytes (default 200 KB)
  double   m_exitK;           //!< Anomaly multiplier k (default 2.0)
  uint32_t m_rttWindowSize;   //!< Rolling window size for RTT stats (default 8)

  // ── Per-Connection State ───────────────────────────────────────────────────

  // C1
  bool   m_iwSet;             //!< True after CongestionStateSet fires once

  // C2
  uint32_t m_rttCount;        //!< RTTs completed since connection open
  double   m_bytesAckedRtt1;  //!< Cumulative bytes ACKed at end of RTT 1
  double   m_bytesAckedRtt2;  //!< Cumulative bytes ACKed at end of RTT 2
  double   m_totalBytesAcked; //!< Running total bytes ACKed
  bool     m_isElephant;      //!< True once classifier labels flow Elephant
  bool     m_classifierFired; //!< True after C2 has run (avoids re-running)

  // C3
  std::deque<double> m_rttSamples; //!< Sliding window of per-RTT RTT values
  double   m_rttMu;           //!< Rolling mean RTT (seconds)
  double   m_rttSigma;        //!< Rolling standard deviation of RTT (seconds)
  double   m_rttSumSq;        //!< Sum of squares for online σ computation
  double   m_rttSum;          //!< Sum for online µ computation
  bool     m_rttStatsReady;   //!< True after ≥2 RTT samples collected
  Time     m_lastRttBoundary; //!< Timestamp of last RTT boundary crossing
  bool     m_inAfsSlowStart;  //!< True while AFS slow-start logic is active

  // ── Shared Bandwidth Cache (static, persists across connections) ───────────
  //    Keyed on /24 subnet prefix derived from peer IPv4 address.
  //    In ns-3, peer address can be obtained from the socket; we use a
  //    simplified hash for demonstration. In a real kernel module this
  //    would be a kernel hash table.
  static std::unordered_map<uint32_t, double> s_bwCache; //!< bps estimates
};

} // namespace ns3

#endif /* TCP_AFS_H */