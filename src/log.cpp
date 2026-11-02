// SPDX-License-Identifier: AGPL-3.0-only
#include "log.h"

#include <cstdarg>
#include <ctime>

static const size_t kMaxLines = 600;

Log &Log::Get()
{
	static Log instance;
	return instance;
}

Log::Log()
{
	chiaki_log_init(&chiaki_log, CHIAKI_LOG_ALL & ~(CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG), ChiakiCb, this);
}

void Log::Open(const std::string &path)
{
	std::lock_guard<std::mutex> lock(mutex);
	if(file)
		fclose(file);
	file = fopen(path.c_str(), "w");
}

void Log::ChiakiCb(ChiakiLogLevel level, const char *msg, void *user)
{
	static_cast<Log *>(user)->Write(level, msg);
}

void Log::Write(ChiakiLogLevel level, const char *msg)
{
	char stamp[32];
	time_t now = time(nullptr);
	struct tm tm_now;
#ifdef _WIN32
	localtime_s(&tm_now, &now);
#else
	localtime_r(&now, &tm_now);
#endif
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm_now);

	std::string line = std::string(stamp) + " [" + chiaki_log_level_char(level) + "] " + msg;
	while(!line.empty() && (line.back() == '\n' || line.back() == '\r'))
		line.pop_back();

	std::lock_guard<std::mutex> lock(mutex);
	lines.push_back(line);
	while(lines.size() > kMaxLines)
		lines.pop_front();
	if(file)
	{
		fputs(line.c_str(), file);
		fputc('\n', file);
		fflush(file);
	}
}

void Log::Printf(ChiakiLogLevel level, const char *fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	Write(level, buf);
}

std::deque<std::string> Log::Recent()
{
	std::lock_guard<std::mutex> lock(mutex);
	return lines;
}
