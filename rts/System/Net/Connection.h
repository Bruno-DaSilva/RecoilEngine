/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#pragma once

#include <string>
#include <memory>

#include "RawPacket.h"

namespace netcode
{

/**
 * @brief per-link traffic statistics
 *
 * Byte counts are tracked by every connection type; everything else is UDP-only
 * -- see isNetworkLink.
 */
struct ConnectionStats {
	// cumulative
	unsigned int sentBytes = 0;
	unsigned int receivedBytes = 0;
	unsigned int sentPackets = 0;
	unsigned int receivedPackets = 0;
	/// chunks we put back on the wire after they had already been sent once
	unsigned int retransmittedChunks = 0;
	/// inbound chunks discarded because the same chunk had already arrived
	unsigned int discardedChunks = 0;
	/// socket-level failures; ours or the environment's, not the peer's link
	unsigned int sendErrors = 0;
	unsigned int receiveErrors = 0;

	// instantaneous
	float sendRateBytesPerSec = 0.0f;
	unsigned int unackedChunks = 0;
	unsigned int queuedResendChunks = 0;
	/// inbound chunks held because an earlier chunk has not arrived
	unsigned int queuedInboundChunks = 0;
	/// application bytes handed to the link and not yet on the wire
	unsigned int queuedSendBytes = 0;

	/// whether the other fields describe a real network link. False on a
	/// loopback, which moves bytes but reports no packets, loss or queue depths,
	/// so its zeroes are missing data rather than a healthy link.
	bool isNetworkLink = false;
};

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
	unsigned int GetNumQueuedPings() const { return numPings; }
	virtual unsigned int GetPacketQueueSize() const { return 0; }

	virtual std::string Statistics() const = 0;
	virtual std::string GetFullAddress() const = 0;
	virtual void Unmute() = 0;
	virtual void Close(bool flush = false) = 0;
	virtual void SetLossFactor(int factor) = 0;

	/**
	 * @brief update internals
	 * Check for unack'd packets, timeout etc.
	 */
	virtual void Update() {}

protected:
	unsigned int dataSent = 0;
	unsigned int dataRecv = 0;
	unsigned int numPings = 0;
};

} // namespace netcode
