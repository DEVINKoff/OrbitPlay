// SPDX-License-Identifier: AGPL-3.0-only
#include "discovery.h"
#include "log.h"
#include "settings.h"

#include <chiaki/discovery.h>
#include <chiaki/time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#endif

#include <cstring>

static const uint64_t kPingMs = 500;
static const size_t kHostsMax = 16;
static const uint64_t kDropPings = 6;

Discovery::~Discovery()
{
	Stop();
}

// All directed broadcast addresses of the PC's active IPv4 adapters. Windows only sends
// 255.255.255.255 out of one interface, so a PC with VPN / Hyper-V adapters may otherwise
// never reach the console.
static std::vector<uint32_t> AdapterBroadcastAddresses()
{
	std::vector<uint32_t> result;
#ifdef _WIN32
	ULONG size = 16 * 1024;
	std::vector<uint8_t> buf(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	if(r == ERROR_BUFFER_OVERFLOW)
	{
		buf.resize(size);
		r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	}
	if(r != NO_ERROR)
		return result;
	for(auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next)
	{
		if(a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for(auto *u = a->FirstUnicastAddress; u; u = u->Next)
		{
			if(u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			uint32_t ip = ntohl(((sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
			uint8_t prefix = u->OnLinkPrefixLength;
			if(prefix == 0 || prefix >= 31)
				continue;
			uint32_t mask = prefix >= 32 ? 0xffffffffu : ~(0xffffffffu >> prefix);
			uint32_t bcast = htonl(ip | ~mask);
			bool dup = false;
			for(uint32_t b : result)
				dup |= b == bcast;
			if(!dup)
				result.push_back(bcast);
		}
	}
#endif
	return result;
}

bool Discovery::Start()
{
	if(running)
		return true;

	ChiakiDiscoveryServiceOptions options = {};
	options.ping_ms = kPingMs;
	options.ping_initial_ms = kPingMs;
	options.hosts_max = kHostsMax;
	options.host_drop_pings = kDropPings;
	options.cb = ServiceCb;
	options.cb_user = this;

	sockaddr_in in_addr = {};
	in_addr.sin_family = AF_INET;
	in_addr.sin_addr.s_addr = 0xffffffff;
	sockaddr_storage addr = {};
	memcpy(&addr, &in_addr, sizeof(in_addr));
	options.send_addr = &addr;
	options.send_addr_size = sizeof(in_addr);

	std::vector<uint32_t> bcasts = AdapterBroadcastAddresses();
	std::vector<sockaddr_storage> bcast_storage(bcasts.size());
	for(size_t i = 0; i < bcasts.size(); i++)
	{
		sockaddr_in b = {};
		b.sin_family = AF_INET;
		b.sin_addr.s_addr = bcasts[i];
		memcpy(&bcast_storage[i], &b, sizeof(b));
	}
	options.broadcast_addrs = bcast_storage.empty() ? nullptr : bcast_storage.data();
	options.broadcast_num = bcast_storage.size();

	ChiakiErrorCode err = chiaki_discovery_service_init(&service, &options, Log::Get().Chiaki());
	if(err != CHIAKI_ERR_SUCCESS)
	{
		LOGE("Console discovery failed to start: %s", chiaki_error_string(err));
		return false;
	}
	running = true;
	LOGI("Console discovery started (%zu adapter broadcast address(es))", bcasts.size());
	return true;
}

void Discovery::Stop()
{
	if(!running)
		return;
	chiaki_discovery_service_fini(&service);
	running = false;
}

void Discovery::ServiceCb(ChiakiDiscoveryHost *hosts, size_t count, void *user)
{
	static_cast<Discovery *>(user)->Merge(hosts, count);
}

static std::string S(const char *s) { return s ? s : ""; }

void Discovery::Merge(ChiakiDiscoveryHost *list, size_t count)
{
	std::vector<DiscoveredHost> fresh;
	uint64_t now = chiaki_time_now_monotonic_ms();
	for(size_t i = 0; i < count; i++)
	{
		ChiakiDiscoveryHost *h = &list[i];
		DiscoveredHost d;
		d.mac = NormalizeMac(S(h->host_id));
		d.address = S(h->host_addr);
		d.name = S(h->host_name);
		d.system_version = S(h->system_version);
		d.running_app = S(h->running_app_name);
		d.running_title_id = S(h->running_app_titleid);
		d.ps5 = chiaki_discovery_host_is_ps5(h);
		d.ready = h->state == CHIAKI_DISCOVERY_HOST_STATE_READY;
		d.target = chiaki_discovery_host_system_version_target(h);
		d.last_seen_ms = now;
		fresh.push_back(d);
	}
	std::lock_guard<std::mutex> lock(mutex);
	hosts = std::move(fresh);
}

std::vector<DiscoveredHost> Discovery::Hosts()
{
	std::lock_guard<std::mutex> lock(mutex);
	return hosts;
}

bool Discovery::Find(const std::string &mac, DiscoveredHost *out)
{
	std::lock_guard<std::mutex> lock(mutex);
	for(const auto &h : hosts)
		if(h.mac == mac)
		{
			*out = h;
			return true;
		}
	return false;
}

bool Discovery::FindByAddress(const std::string &address, DiscoveredHost *out)
{
	std::lock_guard<std::mutex> lock(mutex);
	for(const auto &h : hosts)
		if(h.address == address)
		{
			*out = h;
			return true;
		}
	return false;
}

void Discovery::ProbeAddress(const std::string &address)
{
	if(!running || address.empty())
		return;
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS5);
	if(inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1)
		return;
	ChiakiDiscoveryPacket packet = {};
	packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
	packet.protocol_version = (char *)CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5;
	chiaki_discovery_send(&service.discovery, &packet, (sockaddr *)&addr, sizeof(addr));
}

bool Discovery::Wakeup(const std::string &address, const uint8_t regist_key[16], bool ps5)
{
	// The wakeup credential is the regist key (an ASCII hex string padded with zeros) parsed as hex.
	char key[17] = {};
	memcpy(key, regist_key, 16);
	size_t len = strnlen(key, 16);
	if(len == 0 || len > 16)
	{
		LOGE("Saved regist key is not valid for wakeup");
		return false;
	}
	char *end = nullptr;
	uint64_t credential = strtoull(key, &end, 16);
	if(end != key + len)
	{
		LOGE("Saved regist key is not valid for wakeup");
		return false;
	}
	ChiakiErrorCode err = chiaki_discovery_wakeup(Log::Get().Chiaki(), nullptr, address.c_str(), credential, ps5);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		LOGE("Wakeup packet failed: %s", chiaki_error_string(err));
		return false;
	}
	LOGI("Sent wakeup to %s", address.c_str());
	return true;
}
