// Orbit Play - PS5 Remote Play client for Windows
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chiaki/log.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

// Thread-safe logger: writes to a file in %APPDATA%\OrbitPlay and keeps a
// ring buffer of recent lines for the in-app log viewer.
class Log
{
public:
	static Log &Get();

	void Open(const std::string &path);
	void Write(ChiakiLogLevel level, const char *msg);
	void Printf(ChiakiLogLevel level, const char *fmt, ...);

	// Snapshot of recent lines (newest last).
	std::deque<std::string> Recent();

	ChiakiLog *Chiaki() { return &chiaki_log; }

private:
	Log();
	static void ChiakiCb(ChiakiLogLevel level, const char *msg, void *user);

	std::mutex mutex;
	std::deque<std::string> lines;
	FILE *file = nullptr;
	ChiakiLog chiaki_log;
};

#define LOGI(...) Log::Get().Printf(CHIAKI_LOG_INFO, __VA_ARGS__)
#define LOGW(...) Log::Get().Printf(CHIAKI_LOG_WARNING, __VA_ARGS__)
#define LOGE(...) Log::Get().Printf(CHIAKI_LOG_ERROR, __VA_ARGS__)
