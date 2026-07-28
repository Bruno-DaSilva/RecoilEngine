/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef _CONNECTION_H
#define _CONNECTION_H

#include <atomic>
#include <string>
#include <memory>

#include "RawPacket.h"
#include "System/SafeUtil.h"
#include "System/SpringFormat.h"

namespace netcode
{

/**
 * @brief per-link traffic counters
 *
 * Single extension point for link telemetry: add a field here and fill it in
 * GetStats rather than adding another virtual getter. Byte counts are tracked by
 * every connection type; the rest is UDP-only -- see isNetworkLink.
 */
struct ConnectionStats {
	unsigned int sentBytes = 0;
	unsigned int receivedBytes = 0;
	unsigned int sentPackets = 0;
	unsigned int receivedPackets = 0;
	/// chunks put back on the wire after having been sent once
	unsigned int retransmittedChunks = 0;
	/// retransmitted purely because the link duplicates by policy
	unsigned int duplicatedChunks = 0;
	/// chunks discarded on arrival because the same chunk had already been received
	unsigned int discardedChunks = 0;
	/// seen missing at a send pass; long-lived reordering is indistinguishable
	/// from loss and counts here too
	unsigned int missingChunks = 0;
	/// protocol header bytes, against the payload bytes above
	unsigned int sentOverheadBytes = 0;
	unsigned int receivedOverheadBytes = 0;
	/// inbound chunks delivered in order so far
	unsigned int processedChunks = 0;
	/// socket-level failures; ours or the environment's, not the peer's link
	unsigned int sendErrors = 0;
	unsigned int receiveErrors = 0;
	/// time sending was blocked by the outgoing bandwidth cap
	double sendBlockedMs = 0.0;
	/// time inbound delivery was stalled behind a missing chunk
	double receiveStalledMs = 0.0;

	// current state
	float sendRateBytesPerSec = 0.0f;
	unsigned int unackedChunks = 0;
	unsigned int queuedResendChunks = 0;
	/// received but undeliverable until an earlier chunk arrives
	unsigned int queuedInboundChunks = 0;
	/// handed to the link and not yet on the wire
	unsigned int queuedSendBytes = 0;
	/// loss factor the link is running with, already clamped to the range the
	/// transport accepts
	unsigned int lossFactor = 0;

	/// whether the fields beyond the byte counts describe anything. False on a
	/// loopback, which moves bytes but has no wire to report on.
	bool isNetworkLink = false;
};


/// human-readable dump of a link's counters, for the disconnect log
inline std::string FormatConnectionStats(const ConnectionStats& s)
{
	if (!s.isNetworkLink) {
		return spring::format("\t%u bytes sent\n\t%u bytes recv'd\n", s.sentBytes, s.receivedBytes);
	}

	std::string msg;
	msg += spring::format("\t%u bytes sent   in %u packets (%.3f bytes/packet)\n",
		s.sentBytes, s.sentPackets, spring::SafeDivide(s.sentBytes * 1.0f, s.sentPackets * 1.0f));
	msg += spring::format("\t%u bytes recv'd in %u packets (%.3f bytes/packet)\n",
		s.receivedBytes, s.receivedPackets, spring::SafeDivide(s.receivedBytes * 1.0f, s.receivedPackets * 1.0f));
	msg += spring::format("\t{%.3fx, %.3fx} relative protocol overhead {up, down}\n",
		spring::SafeDivide(s.sentOverheadBytes * 1.0f, s.sentBytes * 1.0f),
		spring::SafeDivide(s.receivedOverheadBytes * 1.0f, s.receivedBytes * 1.0f));
	msg += spring::format("\t%u incoming chunks dropped, %u outgoing chunks resent\n",
		s.discardedChunks, s.retransmittedChunks);
	msg += spring::format("\t%u incoming chunks processed\n", s.processedChunks);
	return msg;
}


/**
 * @brief Base class for connecting to various receivers / senders
 */
class CConnection
{
public:
	virtual ~CConnection() {}

	/**
	 * @brief Send packet to other instance
	 *
	 * Use this, since it does not need memcpy'ing
	 */
	virtual void SendData(std::shared_ptr<const RawPacket> data) = 0;

	virtual bool HasIncomingData() const = 0;

	/**
	 * @brief Take a look at the messages that will be returned by GetData().
	 * @return A RawPacket holding the data, or 0 if no data
	 * @param ahead How many packets to look ahead. A typical usage would be:
	 *   for (int ahead = 0; (packet = conn->Peek(ahead)); ++ahead) {}
	 */
	virtual std::shared_ptr<const RawPacket> Peek(unsigned ahead) const = 0;

	/**
	 * @brief use this to receive ready data
	 * @return a network message encapsulated in a RawPacket,
	 *   or NULL if there are no more messages available.
	 */
	virtual std::shared_ptr<const RawPacket> GetData() = 0;

	/**
	 * @brief Deletes a packet from the buffer
	 * @param index queue index number
	 * Useful for messages that skip queuing and need to be processed
	 * immediately.
	 */
	virtual void DeleteBufferPacketAt(unsigned index) = 0;

	/**
	 * Flushes the underlying buffer (to the network), if there is a buffer.
	 */
	virtual void Flush(const bool forced = false) = 0;
	virtual bool CheckTimeout(int seconds = 0, bool initial = false) const = 0;

	virtual void ReconnectTo(CConnection& conn) = 0;
	virtual bool CanReconnect() const = 0;
	virtual bool NeedsReconnect() = 0;

	unsigned int GetDataReceived() const { return dataRecv; }
	virtual ConnectionStats GetStats() const { return {dataSent, dataRecv}; }

	/**
	 * @brief whether links should collect the optional telemetry in ConnectionStats
	 *
	 * Byte and packet counters are always kept; the rest exists purely to be
	 * exported, so it is off unless something is exporting.
	 *
	 * Atomic because a host client's own CNetProtocol link is already being
	 * serviced when the server it just started flips this.
	 */
	static void SetStatsSampling(bool enable) { statsSampling.store(enable, std::memory_order_relaxed); }
	static bool StatsSampling() { return statsSampling.load(std::memory_order_relaxed); }
	unsigned int GetNumQueuedPings() const { return numPings; }
	virtual unsigned int GetPacketQueueSize() const { return 0; }

	/// one formatter for every link type; GetStats is the extension point
	std::string Statistics() const {
		return "[" + GetFullAddress() + "]\n" + FormatConnectionStats(GetStats());
	}
	virtual std::string GetFullAddress() const = 0;
	virtual void Unmute() = 0;
	virtual void Close(bool flush = false) = 0;
	virtual void SetLossFactor(int factor) = 0;

	/**
	 * @brief update internals
	 * Check for unack'd packets, timeout etc.
	 */
	virtual void Update() {}

private:
	inline static std::atomic<bool> statsSampling = false;

protected:
	unsigned int dataSent = 0;
	unsigned int dataRecv = 0;
	unsigned int numPings = 0;
};

} // namespace netcode

#endif // _CONNECTION_H

