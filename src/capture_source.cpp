// SPDX-License-Identifier: AGPL-3.0-only
#include "capture_source.h"

#ifdef _WIN32
#include "audio.h"
#include "log.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <cctype>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {

template<class T> void SafeRelease(T *&p)
{
	if(p)
	{
		p->Release();
		p = nullptr;
	}
}

std::string Utf8(const wchar_t *w)
{
	if(!w)
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if(n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

uint64_t NowMs() { return GetTickCount64(); }

std::string SubtypeName(const GUID &g)
{
	if(g == MFVideoFormat_NV12) return "NV12";
	if(g == MFVideoFormat_YUY2) return "YUY2";
	if(g == MFVideoFormat_UYVY) return "UYVY";
	if(g == MFVideoFormat_MJPG) return "MJPG";
	if(g == MFVideoFormat_RGB32) return "RGB32";
	if(g == MFVideoFormat_RGB24) return "RGB24";
	if(g == MFVideoFormat_P010) return "P010";
	if(g == MFVideoFormat_I420) return "I420";
	if(g == MFVideoFormat_H264) return "H264";
	char b[8] = {};
	memcpy(b, &g.Data1, 4); // FOURCC subtypes keep the code in Data1
	for(char &c : b)
		if(c && !isprint((unsigned char)c))
			c = '?';
	return b;
}

// Uncompressed formats first: no decoder in the way, lowest latency.
int SubtypeRank(const GUID &g)
{
	if(g == MFVideoFormat_NV12) return 5;
	if(g == MFVideoFormat_YUY2 || g == MFVideoFormat_UYVY) return 4;
	if(g == MFVideoFormat_I420) return 3;
	if(g == MFVideoFormat_RGB32 || g == MFVideoFormat_RGB24) return 2;
	if(g == MFVideoFormat_MJPG) return 1;
	return 0;
}

struct MfInit
{
	bool ok = false;
	MfInit() { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
	~MfInit() { if(ok) MFShutdown(); }
};

std::vector<std::string> Words(const std::string &s)
{
	std::vector<std::string> out;
	std::string cur;
	for(char c : s)
	{
		if(isalnum((unsigned char)c))
			cur += (char)tolower((unsigned char)c);
		else if(!cur.empty())
		{
			out.push_back(cur);
			cur.clear();
		}
	}
	if(!cur.empty())
		out.push_back(cur);
	return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Devices

std::vector<CaptureDevice> CaptureSource::EnumerateVideo()
{
	std::vector<CaptureDevice> out;
	HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	{
		MfInit mf;
		IMFAttributes *attr = nullptr;
		IMFActivate **devices = nullptr;
		UINT32 count = 0;
		if(SUCCEEDED(MFCreateAttributes(&attr, 1))
			&& SUCCEEDED(attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
			&& SUCCEEDED(MFEnumDeviceSources(attr, &devices, &count)))
		{
			for(UINT32 i = 0; i < count; i++)
			{
				WCHAR *name = nullptr, *link = nullptr;
				UINT32 len = 0;
				devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
				devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
				if(link)
					out.push_back({ name ? Utf8(name) : "Capture device", link });
				CoTaskMemFree(name);
				CoTaskMemFree(link);
				devices[i]->Release();
			}
			CoTaskMemFree(devices);
		}
		SafeRelease(attr);
	}
	if(SUCCEEDED(co))
		CoUninitialize();
	return out;
}

std::vector<std::string> CaptureSource::EnumerateAudio()
{
	std::vector<std::string> out;
	int n = 0;
	if(SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&n))
	{
		for(int i = 0; i < n; i++)
			if(const char *name = SDL_GetAudioDeviceName(ids[i]))
				out.push_back(name);
		SDL_free(ids);
	}
	return out;
}

// Capture cards show up as a camera and a microphone that share words in their names
// ("Elgato HD60 X" / "Game Capture HD60 X Audio", "USB Video" / "Digital Audio Interface (USB
// Capture)"). Pick the recording device sharing the most distinctive words.
std::string CaptureSource::GuessAudio(const std::string &video_name)
{
	static const char *generic[] = { "video", "audio", "usb", "device", "input", "capture", "microphone", "mic", "line",
		"digital", "interface", "high", "definition", "realtek", "array", "the", "and" };
	auto is_generic = [&](const std::string &w) {
		for(const char *g : generic)
			if(w == g)
				return true;
		return w.size() < 2;
	};
	std::vector<std::string> vw = Words(video_name);
	std::string best;
	int best_score = 0;
	for(const std::string &a : EnumerateAudio())
	{
		int score = 0;
		std::vector<std::string> aw = Words(a);
		for(const std::string &w : aw)
			for(const std::string &v : vw)
				if(w == v)
					score += is_generic(w) ? 0 : 3;
		// the cheap USB 3.0 / MS2109 sticks: "USB Video" + "Digital Audio Interface (... Capture)"
		std::string la = a;
		std::transform(la.begin(), la.end(), la.begin(), ::tolower);
		if(la.find("capture") != std::string::npos || la.find("hdmi") != std::string::npos)
			score += 2;
		if(score > best_score)
		{
			best_score = score;
			best = a;
		}
	}
	return best_score >= 2 ? best : std::string();
}

// ---------------------------------------------------------------------------
// Capture

CaptureSource::~CaptureSource() { Stop(); }

bool CaptureSource::Start(const CaptureDevice &dev, const std::string &audio_device, Audio *aud, HANDLE ev,
	std::function<void(bool)> frame_cb, std::function<void(const std::string &)> error_cb, std::string *error)
{
	Stop();
	device = dev;
	audio = aud;
	frame_event = ev;
	on_frame = std::move(frame_cb);
	on_error = std::move(error_cb);
	stop = false;
	open_ok = false;
	open_error.clear();
	frames = fps_frames = 0;
	fps = 0.0;
	fps_start_ms = NowMs();
	opened_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	thread = std::thread([this] { Thread(); });
	// Opening a device takes a few hundred milliseconds; some drivers take seconds.
	if(WaitForSingleObject(opened_event, 10000) != WAIT_OBJECT_0)
		open_error = "The capture card didn't respond. Unplug it, plug it back in and try again.";
	if(!open_ok)
	{
		if(error)
			*error = open_error.empty() ? "Couldn't open the capture card." : open_error;
		Stop();
		return false;
	}
	StartAudio(audio_device);
	return true;
}

void CaptureSource::Stop()
{
	stop = true;
	if(thread.joinable())
		thread.join();
	if(audio_stream)
	{
		SDL_DestroyAudioStream(audio_stream);
		audio_stream = nullptr;
	}
	if(opened_event)
	{
		CloseHandle(opened_event);
		opened_event = nullptr;
	}
	std::lock_guard<std::mutex> lock(mutex);
	if(latest)
		av_frame_free(&latest);
}

AVFrame *CaptureSource::PullFrame()
{
	std::lock_guard<std::mutex> lock(mutex);
	AVFrame *f = latest;
	latest = nullptr;
	return f;
}

CaptureFormat CaptureSource::Format()
{
	std::lock_guard<std::mutex> lock(mutex);
	return format;
}

double CaptureSource::Fps()
{
	std::lock_guard<std::mutex> lock(mutex);
	return fps;
}

bool CaptureSource::Open(std::string *error)
{
	IMFAttributes *attr = nullptr;
	HRESULT hr = MFCreateAttributes(&attr, 2);
	if(SUCCEEDED(hr))
		hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	if(SUCCEEDED(hr))
		hr = attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, device.link.c_str());
	if(SUCCEEDED(hr))
		hr = MFCreateDeviceSource(attr, &source);
	SafeRelease(attr);
	if(FAILED(hr))
	{
		*error = "Couldn't open the capture card (it may be in use by OBS or another app).";
		LOGE("MFCreateDeviceSource failed: 0x%08lx", (unsigned long)hr);
		return false;
	}

	// Pick the device's own mode: 60 fps (or more) first, then the largest picture up to 4K,
	// then a format that needs no decoding. Cards like the HD60 X offer 4K30 next to 1080p60;
	// for games the 60 fps mode is the right one, and frame generation takes it from there.
	IMFPresentationDescriptor *pd = nullptr;
	IMFStreamDescriptor *sd = nullptr;
	IMFMediaTypeHandler *handler = nullptr;
	BOOL selected = FALSE;
	if(SUCCEEDED(source->CreatePresentationDescriptor(&pd)) && SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd))
		&& SUCCEEDED(sd->GetMediaTypeHandler(&handler)))
	{
		DWORD count = 0;
		handler->GetMediaTypeCount(&count);
		IMFMediaType *best = nullptr;
		double best_score = -1;
		for(DWORD i = 0; i < count; i++)
		{
			IMFMediaType *t = nullptr;
			if(FAILED(handler->GetMediaTypeByIndex(i, &t)))
				continue;
			GUID sub = {};
			UINT32 w = 0, h = 0, num = 0, den = 1;
			t->GetGUID(MF_MT_SUBTYPE, &sub);
			MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
			MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den);
			double rate = den ? (double)num / den : 0.0;
			int rank = SubtypeRank(sub);
			if(rank == 0 || w == 0 || h == 0 || w > 3840 || h > 2160)
			{
				t->Release();
				continue;
			}
			double fps_tier = rate >= 59.0 ? 2 : rate >= 29.0 ? 1 : 0;
			double score = fps_tier * 1e9 + (double)w * h * 10 + std::min(rate, 240.0) * 2 + rank;
			if(score > best_score)
			{
				best_score = score;
				SafeRelease(best);
				best = t;
			}
			else
				t->Release();
		}
		if(best)
		{
			GUID sub = {};
			UINT32 w = 0, h = 0, num = 0, den = 1;
			best->GetGUID(MF_MT_SUBTYPE, &sub);
			MFGetAttributeSize(best, MF_MT_FRAME_SIZE, &w, &h);
			MFGetAttributeRatio(best, MF_MT_FRAME_RATE, &num, &den);
			if(SUCCEEDED(handler->SetCurrentMediaType(best)))
			{
				std::lock_guard<std::mutex> lock(mutex);
				format.width = (int)w;
				format.height = (int)h;
				format.fps = den ? (double)num / den : 0.0;
				format.subtype = SubtypeName(sub);
			}
			LOGI("Capture mode: %ux%u at %.2f fps, %s", w, h, den ? (double)num / den : 0.0, SubtypeName(sub).c_str());
			best->Release();
		}
	}
	SafeRelease(handler);
	SafeRelease(sd);
	SafeRelease(pd);

	// The source reader converts whatever the card sends to NV12 (and decodes MJPEG).
	hr = MFCreateAttributes(&attr, 3);
	if(SUCCEEDED(hr))
	{
		attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
		attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		attr->SetUINT32(MF_LOW_LATENCY, TRUE);
		hr = MFCreateSourceReaderFromMediaSource(source, attr, &reader);
	}
	SafeRelease(attr);
	if(FAILED(hr))
	{
		*error = "Couldn't start reading from the capture card.";
		LOGE("MFCreateSourceReaderFromMediaSource failed: 0x%08lx", (unsigned long)hr);
		return false;
	}
	reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
	IMFMediaType *out = nullptr;
	hr = MFCreateMediaType(&out);
	if(SUCCEEDED(hr))
	{
		out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
	}
	SafeRelease(out);
	if(FAILED(hr))
	{
		*error = "This capture card's picture format isn't supported (" + Format().subtype + ").";
		LOGE("Capture: NV12 output not available: 0x%08lx", (unsigned long)hr);
		return false;
	}
	return true;
}

void CaptureSource::Thread()
{
	HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	MfInit mf;
	std::string err;
	open_ok = mf.ok && Open(&err);
	open_error = err;
	SetEvent(opened_event);
	bool first = true;
	bool failed = false;

	auto read_output_type = [&]() {
		IMFMediaType *t = nullptr;
		if(SUCCEEDED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &t)))
		{
			UINT32 w = 0, h = 0;
			MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
			out_w = (int)w;
			out_h = (int)h;
			UINT32 st = 0;
			stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)(INT32)st : (LONG)w;
			t->Release();
		}
	};
	if(open_ok)
		read_output_type();

	while(open_ok && !stop)
	{
		DWORD index = 0, flags = 0;
		LONGLONG ts = 0;
		IMFSample *sample = nullptr;
		HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &index, &flags, &ts, &sample);
		if(FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
		{
			SafeRelease(sample);
			LOGE("Capture stopped: 0x%08lx flags 0x%lx", (unsigned long)hr, (unsigned long)flags);
			failed = true;
			break;
		}
		if(flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
			read_output_type();
		if(!sample)
			continue;
		IMFMediaBuffer *buf = nullptr;
		if(SUCCEEDED(sample->GetBufferByIndex(0, &buf)) && out_w > 0 && out_h > 0)
		{
			BYTE *data = nullptr;
			LONG pitch = 0;
			IMF2DBuffer *b2 = nullptr;
			bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&data, &pitch));
			bool locked = false;
			if(!locked2d)
			{
				SafeRelease(b2);
				locked = SUCCEEDED(buf->Lock(&data, nullptr, nullptr));
				pitch = stride > 0 ? stride : out_w;
			}
			if((locked2d || locked) && data && pitch > 0)
			{
				// NV12: Y rows, then interleaved UV rows. Some drivers pad the Y plane to a
				// multiple of 16 lines; with a plain buffer the padding shows in the length.
				int y_rows = out_h;
				DWORD cur = 0;
				if(SUCCEEDED(buf->GetCurrentLength(&cur)) && cur)
				{
					int rows = (int)(cur / (DWORD)pitch);
					int aligned = (out_h + 15) & ~15;
					if(rows != out_h * 3 / 2 && rows == aligned * 3 / 2)
						y_rows = aligned;
				}
				AVFrame *f = av_frame_alloc();
				if(!f)
					break;
				f->format = AV_PIX_FMT_NV12;
				f->width = out_w;
				f->height = out_h & ~1;
				if(av_frame_get_buffer(f, 64) == 0)
				{
					for(int y = 0; y < f->height; y++)
						memcpy(f->data[0] + (size_t)y * f->linesize[0], data + (size_t)y * pitch, (size_t)out_w);
					const BYTE *uv = data + (size_t)pitch * y_rows;
					for(int y = 0; y < f->height / 2; y++)
						memcpy(f->data[1] + (size_t)y * f->linesize[1], uv + (size_t)y * pitch, (size_t)(out_w & ~1));
					// HDMI video is limited range; HD and up is BT.709
					f->color_range = AVCOL_RANGE_MPEG;
					bool hd = out_h >= 720;
					f->colorspace = hd ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
					f->color_primaries = hd ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
					f->color_trc = hd ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
					{
						std::lock_guard<std::mutex> lock(mutex);
						if(latest)
							av_frame_free(&latest);
						latest = f;
						frames++;
						fps_frames++;
						uint64_t now = NowMs();
						if(now - fps_start_ms >= 1000)
						{
							fps = fps_frames * 1000.0 / (double)(now - fps_start_ms);
							fps_frames = 0;
							fps_start_ms = now;
						}
						format.width = out_w;
						format.height = f->height;
					}
					if(on_frame)
						on_frame(first);
					first = false;
					if(frame_event)
						SetEvent(frame_event);
				}
				else
					av_frame_free(&f);
			}
			if(locked2d)
				b2->Unlock2D();
			else if(locked)
				buf->Unlock();
			SafeRelease(b2);
		}
		SafeRelease(buf);
		SafeRelease(sample);
	}

	SafeRelease(reader);
	if(source)
	{
		source->Shutdown();
		SafeRelease(source);
	}
	if(failed && !stop && on_error)
		on_error("The capture card stopped (unplugged, or taken by another app).");
	if(SUCCEEDED(co))
		CoUninitialize();
}

// ---------------------------------------------------------------------------
// Audio: the card's recording device straight to the game audio output

void SDLCALL CaptureSource::AudioCb(void *user, SDL_AudioStream *stream, int additional, int)
{
	auto *self = static_cast<CaptureSource *>(user);
	if(additional <= 0 || !self->audio)
		return;
	self->audio_buf.resize((size_t)additional / sizeof(int16_t));
	int got = SDL_GetAudioStreamData(stream, self->audio_buf.data(), additional);
	if(got > 0)
		self->audio->PushGame(self->audio_buf.data(), (size_t)got / (sizeof(int16_t) * 2));
}

void CaptureSource::StartAudio(const std::string &name)
{
	audio_name.clear();
	if(!audio || name.empty())
		return;
	int n = 0;
	SDL_AudioDeviceID id = 0;
	if(SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&n))
	{
		for(int i = 0; i < n; i++)
			if(const char *dn = SDL_GetAudioDeviceName(ids[i]); dn && name == dn)
				id = ids[i];
		SDL_free(ids);
	}
	if(!id)
	{
		LOGW("Capture audio device '%s' not found", name.c_str());
		return;
	}
	audio->OpenGame(2, 48000);
	SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 48000 };
	audio_stream = SDL_OpenAudioDeviceStream(id, &spec, AudioCb, this);
	if(!audio_stream)
	{
		LOGW("Couldn't open capture audio '%s': %s", name.c_str(), SDL_GetError());
		return;
	}
	SDL_ResumeAudioStreamDevice(audio_stream);
	audio_name = name;
	LOGI("Capture audio: %s", name.c_str());
}
#endif
