// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Lossless recording and instant replay.
//
// The PS5 already sends compressed H.264 / HEVC video. The recorder writes exactly those packets
// into an MP4 (no re-encoding: identical quality, almost no CPU/GPU cost), with the game audio
// encoded to AAC. HDR streams keep their HDR10 metadata.
//
// Instant replay keeps the last N seconds in memory (the app asks the PS5 for a keyframe every
// few seconds so a clip can start anywhere) and saves them on demand.
class Recorder
{
public:
	Recorder();
	~Recorder();

	struct StreamInfo
	{
		bool hevc = true;
		bool hdr = false;
		int width = 1920, height = 1080;
		int fps = 60;
	};
	void SetStreamInfo(const StreamInfo &info);

	// Called from chiaki's threads.
	void OnVideoPacket(const uint8_t *data, size_t size);
	void OnAudio(const int16_t *pcm, size_t frames, int channels, int rate);

	// Recording: starts at the next keyframe (the caller requests one from the console).
	bool StartRecording(const std::string &path_utf8);
	void StopRecording();
	bool Recording() const { return recording; }
	double RecordingSeconds() const;

	// Instant replay
	void SetReplay(bool enabled, int seconds);
	bool ReplayEnabled() const { return replay_enabled; }
	double ReplayBufferedSeconds();
	// Saves the buffered replay (written on a background thread).
	bool SaveReplay(const std::string &path_utf8);

	// Last finished file / error, for the UI.
	std::string TakeMessage();

	static bool IsKeyframe(const uint8_t *data, size_t size, bool hevc);

private:
	struct VPacket { std::vector<uint8_t> data; uint64_t t_us; bool key; };
	struct APacket { std::vector<int16_t> pcm; uint64_t t_us; };
	struct Job
	{
		std::string path;
		StreamInfo info;
		int channels = 2, rate = 48000;
		std::deque<VPacket> video;   // starts with a keyframe
		std::deque<APacket> audio;
		bool live = false;           // live recording (more packets keep coming)
	};

	bool WriteJob(Job &job);
	void TrimReplay(uint64_t now_us);
	void SetMessage(const std::string &m);

	std::mutex mutex;
	std::condition_variable cv;
	StreamInfo info;
	int audio_channels = 2, audio_rate = 48000;

	// live recording
	std::atomic<bool> recording{ false };
	bool waiting_key = false;
	uint64_t rec_start_us = 0;
	std::shared_ptr<Job> live_job;
	std::deque<VPacket> live_video;
	std::deque<APacket> live_audio;
	bool live_stop = false;

	// replay ring
	std::atomic<bool> replay_enabled{ false };
	int replay_seconds = 30;
	std::deque<VPacket> ring_video;
	std::deque<APacket> ring_audio;

	// background writers (one thread per file being written)
	bool quit = false;
	int active_writers = 0;

	std::mutex msg_mutex;
	std::string message;
};
