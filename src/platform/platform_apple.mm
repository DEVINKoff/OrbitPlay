// SPDX-License-Identifier: AGPL-3.0-only
// iPadOS platform layer (compiled with ARC).
#import <Foundation/Foundation.h>

#include "platform.h"

#include <SDL3/SDL.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>

namespace platform
{
std::string DataDir()
{
	// Library/Application Support/OrbitPlay (not shown in the Files app)
	char *p = SDL_GetPrefPath("OrbitPlay", "OrbitPlay");
	std::string dir = p ? p : "./";
	SDL_free(p);
	if(!dir.empty() && dir.back() == '/')
		dir.pop_back();
	return dir;
}

std::string CaptureDir(bool)
{
	// Documents: visible in the Files app under On My iPad > Orbit Play (UIFileSharingEnabled)
	const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
	std::string dir = docs ? docs : "./";
	if(!dir.empty() && dir.back() == '/')
		dir.pop_back();
	mkdir(dir.c_str(), 0755);
	return dir;
}

std::string TimestampName(const char *prefix, const char *ext)
{
	time_t t = time(nullptr);
	struct tm lt;
	localtime_r(&t, &lt);
	char b[128];
	snprintf(b, sizeof(b), "%s_%04d%02d%02d_%02d%02d%02d.%s", prefix, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
		lt.tm_min, lt.tm_sec, ext);
	return b;
}

std::string JoinPath(const std::string &dir, const std::string &name) { return dir + "/" + name; }

std::string ClockText()
{
	@autoreleasepool
	{
		static NSDateFormatter *f = nil;
		if(!f)
		{
			f = [NSDateFormatter new];
			f.timeStyle = NSDateFormatterShortStyle;
			f.dateStyle = NSDateFormatterNoStyle;
		}
		return [f stringFromDate:[NSDate date]].UTF8String;
	}
}

void OpenFolder(const std::string &) {}

bool FileExists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }

void RemoveFile(const std::string &path) { unlink(path.c_str()); }

// IBM Plex Sans ships in the app bundle (fonts/)
bool FontData(int weight, const void **data, int *size)
{
	static std::map<int, std::vector<char>> cache;
	auto it = cache.find(weight);
	if(it == cache.end())
	{
		const char *names[] = { "", "Light", "Regular", "Medium", "SemiBold" };
		if(weight < 1 || weight > 4)
			return false;
		const char *base = SDL_GetBasePath();
		std::string path = std::string(base ? base : "") + "fonts/IBMPlexSans-" + names[weight] + ".ttf";
		std::ifstream in(path, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		it = cache.emplace(weight, std::move(bytes)).first;
	}
	if(it->second.empty())
		return false;
	*data = it->second.data();
	*size = (int)it->second.size();
	return true;
}

std::string IconFontPath() { return ""; }

// iPadOS only lets apps broadcast with a special Apple entitlement, so consoles are found by asking
// every address of the local /24 network directly (unicast is allowed after the Local Network prompt).
std::vector<std::string> LocalSubnetHosts()
{
	std::vector<std::string> out;
	struct ifaddrs *ifs = nullptr;
	if(getifaddrs(&ifs) != 0)
		return out;
	for(struct ifaddrs *i = ifs; i; i = i->ifa_next)
	{
		if(!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		std::string name = i->ifa_name ? i->ifa_name : "";
		if(name.rfind("en", 0) != 0) // Wi-Fi / Ethernet adapters
			continue;
		uint32_t ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
		uint32_t base = ip & 0xFFFFFF00u;
		for(uint32_t h = 1; h < 255; h++)
		{
			uint32_t a = base | h;
			if(a == ip)
				continue;
			char b[32];
			snprintf(b, sizeof(b), "%u.%u.%u.%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
			out.push_back(b);
		}
		break;
	}
	freeifaddrs(ifs);
	return out;
}

const char *Name() { return "iPadOS"; }
}
