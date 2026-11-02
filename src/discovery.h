// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chiaki/discoveryservice.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct DiscoveredHost
{
	std::string mac;           // normalized from host_id
	std::string address;
	std::string name;
	std::string system_version;
	std::string running_app;
	std::string running_title_id;
	bool ps5 = true;
	bool ready = false;        // false = rest mode / standby
	int target = 0;            // ChiakiTarget derived from the system version
	uint64_t last_seen_ms = 0;
};

// Continuously pings the LAN (broadcast on every adapter) for PlayStation consoles.
class Discovery
{
public:
	Discovery() = default;
	~Discovery();

	bool Start();
	void Stop();

	std::vector<DiscoveredHost> Hosts();
	bool Find(const std::string &mac, DiscoveredHost *out);
	bool FindByAddress(const std::string &address, DiscoveredHost *out);

	// Probe a specific IP (for consoles on another subnet / when broadcasts are blocked).
	void ProbeAddress(const std::string &address);

	// Send the Wake-on-LAN style "wakeup" packet used by Remote Play.
	static bool Wakeup(const std::string &address, const uint8_t regist_key[16], bool ps5);

private:
	static void ServiceCb(ChiakiDiscoveryHost *hosts, size_t count, void *user);
	void Merge(ChiakiDiscoveryHost *hosts, size_t count);

	ChiakiDiscoveryService service = {};
	bool running = false;
	std::mutex mutex;
	std::vector<DiscoveredHost> hosts;
};
