/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2017  ZeroTier, Inc.  https://www.zerotier.com/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial closed-source software that incorporates or links
 * directly against ZeroTier software without disclosing the source code
 * of your own application.
 */

#ifndef ZT_PATH_HPP
#define ZT_PATH_HPP

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <stdexcept>
#include <algorithm>

#include "Constants.hpp"
#include "InetAddress.hpp"
#include "SharedPtr.hpp"
#include "AtomicCounter.hpp"
#include "NonCopyable.hpp"
#include "Utils.hpp"

/**
 * Maximum return value of preferenceRank()
 */
#define ZT_PATH_MAX_PREFERENCE_RANK ((ZT_INETADDRESS_MAX_SCOPE << 1) | 1)

namespace ZeroTier {

class RuntimeEnvironment;

/**
 * A path across the physical network
 */
class Path : NonCopyable
{
	friend class SharedPtr<Path>;

public:
	/**
	 * Efficient unique key for paths in a Hashtable
	 */
	class HashKey
	{
	public:
		HashKey() {}

		HashKey(const int64_t l,const InetAddress &r)
		{
			if (r.ss_family == AF_INET) {
				_k[0] = (uint64_t)reinterpret_cast<const struct sockaddr_in *>(&r)->sin_addr.s_addr;
				_k[1] = (uint64_t)reinterpret_cast<const struct sockaddr_in *>(&r)->sin_port;
				_k[2] = (uint64_t)l;
			} else if (r.ss_family == AF_INET6) {
				ZT_FAST_MEMCPY(_k,reinterpret_cast<const struct sockaddr_in6 *>(&r)->sin6_addr.s6_addr,16);
				_k[2] = ((uint64_t)reinterpret_cast<const struct sockaddr_in6 *>(&r)->sin6_port << 32) ^ (uint64_t)l;
			} else {
				ZT_FAST_MEMCPY(_k,&r,std::min(sizeof(_k),sizeof(InetAddress)));
				_k[2] += (uint64_t)l;
			}
		}

		inline unsigned long hashCode() const { return (unsigned long)(_k[0] + _k[1] + _k[2]); }

		inline bool operator==(const HashKey &k) const { return ( (_k[0] == k._k[0]) && (_k[1] == k._k[1]) && (_k[2] == k._k[2]) ); }
		inline bool operator!=(const HashKey &k) const { return (!(*this == k)); }

	private:
		uint64_t _k[3];
	};

	Path() :
		_lastOut(0),
		_lastIn(0),
		_lastTrustEstablishedPacketReceived(0),
		_incomingLinkQualityFastLog(0xffffffffffffffffULL),
		_localSocket(-1),
		_incomingLinkQualitySlowLogPtr(0),
		_incomingLinkQualitySlowLogCounter(-64), // discard first fast log
		_incomingLinkQualityPreviousPacketCounter(0),
		_outgoingPacketCounter(0),
		_latency(0xffff),
		_addr(),
		_ipScope(InetAddress::IP_SCOPE_NONE)
	{
		for(int i=0;i<(int)sizeof(_incomingLinkQualitySlowLog);++i)
			_incomingLinkQualitySlowLog[i] = ZT_PATH_LINK_QUALITY_MAX;
	}

	Path(const int64_t localSocket,const InetAddress &addr) :
		_lastOut(0),
		_lastIn(0),
		_lastTrustEstablishedPacketReceived(0),
		_incomingLinkQualityFastLog(0xffffffffffffffffULL),
		_localSocket(localSocket),
		_incomingLinkQualitySlowLogPtr(0),
		_incomingLinkQualitySlowLogCounter(-64), // discard first fast log
		_incomingLinkQualityPreviousPacketCounter(0),
		_outgoingPacketCounter(0),
		_latency(0xffff),
		_addr(addr),
		_ipScope(addr.ipScope())
	{
		for(int i=0;i<(int)sizeof(_incomingLinkQualitySlowLog);++i)
			_incomingLinkQualitySlowLog[i] = ZT_PATH_LINK_QUALITY_MAX;
	}

	/**
	 * Called when a packet is received from this remote path, regardless of content
	 *
	 * @param t Time of receive
	 */
	inline void received(const uint64_t t) { _lastIn = t; }

	/**
	 * Update link quality using a counter from an incoming packet (or packet head in fragmented case)
	 *
	 * @param counter Packet link quality counter (range 0 to 7, must not have other bits set)
	 */
	inline void updateLinkQuality(const unsigned int counter)
	{
		const unsigned int prev = _incomingLinkQualityPreviousPacketCounter;
		_incomingLinkQualityPreviousPacketCounter = counter;
		const uint64_t fl = (_incomingLinkQualityFastLog = ((_incomingLinkQualityFastLog << 1) | (uint64_t)(prev == ((counter - 1) & 0x7))));
		if (++_incomingLinkQualitySlowLogCounter >= 64) {
			_incomingLinkQualitySlowLogCounter = 0;
			_incomingLinkQualitySlowLog[_incomingLinkQualitySlowLogPtr++ % sizeof(_incomingLinkQualitySlowLog)] = (uint8_t)Utils::countBits(fl);
		}
	}

	/**
	 * @return Link quality from 0 (min) to 255 (max)
	 */
	inline unsigned int linkQuality() const
	{
		unsigned long slsize = _incomingLinkQualitySlowLogPtr;
		if (slsize > (unsigned long)sizeof(_incomingLinkQualitySlowLog))
			slsize = (unsigned long)sizeof(_incomingLinkQualitySlowLog);
		else if (!slsize)
			return 255; // ZT_PATH_LINK_QUALITY_MAX
		unsigned long lq = 0;
		for(unsigned long i=0;i<slsize;++i)
			lq += (unsigned long)_incomingLinkQualitySlowLog[i] * 4;
		lq /= slsize;
		return (unsigned int)((lq >= 255) ? 255 : lq);
	}

	/**
	 * Set time last trusted packet was received (done in Peer::received())
	 */
	inline void trustedPacketReceived(const uint64_t t) { _lastTrustEstablishedPacketReceived = t; }

	/**
	 * Send a packet via this path (last out time is also updated)
	 *
	 * @param RR Runtime environment
	 * @param tPtr Thread pointer to be handed through to any callbacks called as a result of this call
	 * @param data Packet data
	 * @param len Packet length
	 * @param now Current time
	 * @return True if transport reported success
	 */
	bool send(const RuntimeEnvironment *RR,void *tPtr,const void *data,unsigned int len,int64_t now);

	/**
	 * Manually update last sent time
	 *
	 * @param t Time of send
	 */
	inline void sent(const int64_t t) { _lastOut = t; }

	/**
	 * Update path latency with a new measurement
	 *
	 * @param l Measured latency
	 */
	inline void updateLatency(const unsigned int l)
	{
		unsigned int pl = _latency;
		if (pl < 0xffff)
			_latency = (pl + l) / 2;
		else _latency = l;
	}

	/**
	 * @return Local socket as specified by external code
	 */
	inline const int64_t localSocket() const { return _localSocket; }

	/**
	 * @return Physical address
	 */
	inline const InetAddress &address() const { return _addr; }

	/**
	 * @return IP scope -- faster shortcut for address().ipScope()
	 */
	inline InetAddress::IpScope ipScope() const { return _ipScope; }

	/**
	 * @return True if path has received a trust established packet (e.g. common network membership) in the past ZT_TRUST_EXPIRATION ms
	 */
	inline bool trustEstablished(const int64_t now) const { return ((now - _lastTrustEstablishedPacketReceived) < ZT_TRUST_EXPIRATION); }

	/**
	 * @return Preference rank, higher == better
	 */
	inline unsigned int preferenceRank() const
	{
		// This causes us to rank paths in order of IP scope rank (see InetAdddress.hpp) but
		// within each IP scope class to prefer IPv6 over IPv4.
		return ( ((unsigned int)_ipScope << 1) | (unsigned int)(_addr.ss_family == AF_INET6) );
	}

	/**
	 * Check whether this address is valid for a ZeroTier path
	 *
	 * This checks the address type and scope against address types and scopes
	 * that we currently support for ZeroTier communication.
	 *
	 * @param a Address to check
	 * @return True if address is good for ZeroTier path use
	 */
	static inline bool isAddressValidForPath(const InetAddress &a)
	{
		if ((a.ss_family == AF_INET)||(a.ss_family == AF_INET6)) {
			switch(a.ipScope()) {
				/* Note: we don't do link-local at the moment. Unfortunately these
				 * cause several issues. The first is that they usually require a
				 * device qualifier, which we don't handle yet and can't portably
				 * push in PUSH_DIRECT_PATHS. The second is that some OSes assign
				 * these very ephemerally or otherwise strangely. So we'll use
				 * private, pseudo-private, shared (e.g. carrier grade NAT), or
				 * global IP addresses. */
				case InetAddress::IP_SCOPE_PRIVATE:
				case InetAddress::IP_SCOPE_PSEUDOPRIVATE:
				case InetAddress::IP_SCOPE_SHARED:
				case InetAddress::IP_SCOPE_GLOBAL:
					if (a.ss_family == AF_INET6) {
						// TEMPORARY HACK: for now, we are going to blacklist he.net IPv6
						// tunnels due to very spotty performance and low MTU issues over
						// these IPv6 tunnel links.
						const uint8_t *ipd = reinterpret_cast<const uint8_t *>(reinterpret_cast<const struct sockaddr_in6 *>(&a)->sin6_addr.s6_addr);
						if ((ipd[0] == 0x20)&&(ipd[1] == 0x01)&&(ipd[2] == 0x04)&&(ipd[3] == 0x70))
							return false;
					}
					return true;
				default:
					return false;
			}
		}
		return false;
	}

	/**
	 * @return Latency or 0xffff if unknown
	 */
	inline unsigned int latency() const { return _latency; }

	/**
	 * @return Path quality -- lower is better
	 */
	inline long quality(const int64_t now) const
	{
		const int l = (long)_latency;
		const int age = (long)std::min((now - _lastIn),(int64_t)(ZT_PATH_HEARTBEAT_PERIOD * 10)); // set an upper sanity limit to avoid overflow
		return (((age < (ZT_PATH_HEARTBEAT_PERIOD + 5000)) ? l : (l + 0xffff + age)) * (long)((ZT_INETADDRESS_MAX_SCOPE - _ipScope) + 1));
	}

	/**
	 * @return True if this path is alive (receiving heartbeats)
	 */
	inline bool alive(const int64_t now) const { return ((now - _lastIn) < (ZT_PATH_HEARTBEAT_PERIOD + 5000)); }

	/**
	 * @return True if this path needs a heartbeat
	 */
	inline bool needsHeartbeat(const int64_t now) const { return ((now - _lastOut) >= ZT_PATH_HEARTBEAT_PERIOD); }

	/**
	 * @return Last time we sent something
	 */
	inline int64_t lastOut() const { return _lastOut; }

	/**
	 * @return Last time we received anything
	 */
	inline int64_t lastIn() const { return _lastIn; }

	/**
	 * @return Time last trust-established packet was received
	 */
	inline int64_t lastTrustEstablishedPacketReceived() const { return _lastTrustEstablishedPacketReceived; }

	/**
	 * Return and increment outgoing packet counter (used with Packet::armor())
	 *
	 * @return Next value that should be used for outgoing packet counter (only least significant 3 bits are used)
	 */
	inline unsigned int nextOutgoingCounter() { return _outgoingPacketCounter++; }

private:
	volatile int64_t _lastOut;
	volatile int64_t _lastIn;
	volatile int64_t _lastTrustEstablishedPacketReceived;
	volatile uint64_t _incomingLinkQualityFastLog;
	int64_t _localSocket;
	volatile unsigned long _incomingLinkQualitySlowLogPtr;
	volatile signed int _incomingLinkQualitySlowLogCounter;
	volatile unsigned int _incomingLinkQualityPreviousPacketCounter;
	volatile unsigned int _outgoingPacketCounter;
	volatile unsigned int _latency;
	InetAddress _addr;
	InetAddress::IpScope _ipScope; // memoize this since it's a computed value checked often
	volatile uint8_t _incomingLinkQualitySlowLog[32];
	AtomicCounter __refCount;
};

} // namespace ZeroTier

#endif
