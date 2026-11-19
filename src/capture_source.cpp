// SPDX-License-Identifier: AGPL-3.0-only
#include "capture_source.h"

#ifdef _WIN32
#include "audio.h"
#include "capture_convert.h"
#include "log.h"
#include "recorder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
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
// Modes

std::string CaptureMode::Label() const
{
	char b[96];
	snprintf(b, sizeof(b), "%dx%d  %.0f fps  %s%s", width, height, fps, subtype.c_str(), compressed ? " (compressed)" : "");
	return b;
}

std::string CaptureMode::Key() const
{
	char b[64];
	snprintf(b, sizeof(b), "%dx%d@%.3f:%s", width, height, fps, subtype.c_str());
	return b;
}

namespace {

bool Uncompressed(const GUID &g)
{
	return g == MFVideoFormat_YUY2 || g == MFVideoFormat_UYVY || g == MFVideoFormat_NV12;
}

CaptureMode ModeOf(IMFMediaType *t)
{
	CaptureMode m;
	GUID sub = {};
	UINT32 w = 0, h = 0, num = 0, den = 1;
	t->GetGUID(MF_MT_SUBTYPE, &sub);
	MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
	MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den);
	m.width = (int)w;
	m.height = (int)h;
	m.fps = den ? (double)num / den : 0.0;
	m.subtype = SubtypeName(sub);
	m.compressed = !Uncompressed(sub);
	return m;
}

// Best first. The card's own uncompressed picture always beats a compressed one: with MJPG the
// card shrinks the picture into JPEGs (blocky, smeared colour) and may scale it too. Then 60 fps,
// then the largest picture up to 4K.
double ModeScore(const CaptureMode &m, const GUID &sub)
{
	if(SubtypeRank(sub) == 0 || m.width <= 0 || m.height <= 0 || m.width > 3840 || m.height > 2160)
		return -1;
	double fps_tier = m.fps >= 59.0 ? 2 : m.fps >= 29.0 ? 1 : 0;
	return (m.compressed ? 0 : 1e13) + fps_tier * 1e10 + (double)m.width * m.height * 10 + std::min(m.fps, 240.0) * 2
		+ SubtypeRank(sub);
}

IMFMediaSource *OpenSource(const CaptureDevice &dev, HRESULT *out_hr)
{
	IMFAttributes *attr = nullptr;
	IMFMediaSource *src = nullptr;
	HRESULT hr = MFCreateAttributes(&attr, 2);
	if(SUCCEEDED(hr))
		hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	if(SUCCEEDED(hr))
		hr = attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, dev.link.c_str());
	if(SUCCEEDED(hr))
		hr = MFCreateDeviceSource(attr, &src);
	SafeRelease(attr);
	if(out_hr)
		*out_hr = hr;
	return SUCCEEDED(hr) ? src : nullptr;
}

IMFMediaTypeHandler *Handler(IMFMediaSource *src)
{
	IMFPresentationDescriptor *pd = nullptr;
	IMFStreamDescriptor *sd = nullptr;
	IMFMediaTypeHandler *handler = nullptr;
	BOOL selected = FALSE;
	if(SUCCEEDED(src->CreatePresentationDescriptor(&pd)) && SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd)))
		sd->GetMediaTypeHandler(&handler);
	SafeRelease(sd);
	SafeRelease(pd);
	return handler;
}

} // namespace

std::vector<CaptureMode> CaptureSource::EnumerateModes(const CaptureDevice &dev)
{
	std::vector<std::pair<double, CaptureMode>> list;
	HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	{
		MfInit mf;
		if(IMFMediaSource *src = OpenSource(dev, nullptr))
		{
			if(IMFMediaTypeHandler *handler = Handler(src))
			{
				DWORD count = 0;
				handler->GetMediaTypeCount(&count);
				for(DWORD i = 0; i < count; i++)
				{
					IMFMediaType *t = nullptr;
					if(FAILED(handler->GetMediaTypeByIndex(i, &t)))
						continue;
					GUID sub = {};
					t->GetGUID(MF_MT_SUBTYPE, &sub);
					CaptureMode m = ModeOf(t);
					double score = ModeScore(m, sub);
					t->Release();
					if(score < 0)
						continue;
					bool dup = false;
					for(auto &e : list)
						dup |= e.second.Key() == m.Key();
					if(!dup)
						list.push_back({ score, m });
				}
				handler->Release();
			}
			src->Shutdown();
			src->Release();
		}
	}
	if(SUCCEEDED(co))
		CoUninitialize();
	std::stable_sort(list.begin(), list.end(), [](auto &a, auto &b) { return a.first > b.first; });
	std::vector<CaptureMode> out;
	for(auto &e : list)
		out.push_back(e.second);
	return out;
}

// ---------------------------------------------------------------------------
// Capture

CaptureSource::~CaptureSource() { Stop(); }

bool CaptureSource::Start(const CaptureDevice &dev, const Options &o, Audio *aud, HANDLE ev,
	std::function<void(bool)> frame_cb, std::function<void(const std::string &)> error_cb, std::string *error)
{
	Stop();
	device = dev;
	opt = o;
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
	StartAudio(opt.audio_device);
	{
		std::lock_guard<std::mutex> lock(enc_mutex);
		enc_quit = false;
	}
	enc_thread = std::thread([this] { EncoderThread(); });
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
	{
		std::lock_guard<std::mutex> lock(enc_mutex);
		enc_quit = true;
	}
	enc_cv.notify_all();
	if(enc_thread.joinable())
		enc_thread.join();
	{
		std::lock_guard<std::mutex> lock(enc_mutex);
		for(AVFrame *f : enc_queue)
			av_frame_free(&f);
		enc_queue.clear();
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

void CaptureSource::SetRecorder(Recorder *r) { recorder = r; }

bool CaptureSource::Open(std::string *error)
{
	HRESULT hr = S_OK;
	source = OpenSource(device, &hr);
	if(!source)
	{
		*error = "Couldn't open the capture card (it may be in use by OBS or another app).";
		LOGE("MFCreateDeviceSource failed: 0x%08lx", (unsigned long)hr);
		return false;
	}

	// Choose the mode: the saved one if the card still offers it, otherwise the best.
	GUID chosen_sub = {};
	if(IMFMediaTypeHandler *handler = Handler(source))
	{
		DWORD count = 0;
		handler->GetMediaTypeCount(&count);
		IMFMediaType *best = nullptr, *wanted = nullptr;
		double best_score = -1;
		for(DWORD i = 0; i < count; i++)
		{
			IMFMediaType *t = nullptr;
			if(FAILED(handler->GetMediaTypeByIndex(i, &t)))
				continue;
			GUID sub = {};
			t->GetGUID(MF_MT_SUBTYPE, &sub);
			CaptureMode m = ModeOf(t);
			double score = ModeScore(m, sub);
			if(!wanted && !opt.mode_key.empty() && m.Key() == opt.mode_key && score >= 0)
			{
				wanted = t;
				t->AddRef();
			}
			if(score > best_score)
			{
				best_score = score;
				SafeRelease(best);
				best = t;
			}
			else
				t->Release();
		}
		IMFMediaType *use = wanted ? wanted : best;
		if(use)
		{
			use->GetGUID(MF_MT_SUBTYPE, &chosen_sub);
			CaptureMode m = ModeOf(use);
			hr = handler->SetCurrentMediaType(use);
			LOGI("Capture mode: %s%s (0x%08lx)", m.Label().c_str(), wanted ? " (chosen)" : " (automatic)", (unsigned long)hr);
			std::lock_guard<std::mutex> lock(mutex);
			format = m;
		}
		SafeRelease(wanted);
		SafeRelease(best);
		handler->Release();
	}
	if(chosen_sub == GUID{})
	{
		*error = "The capture card offers no picture format Orbit can show.";
		return false;
	}

	// Uncompressed modes are read exactly as the card sends them (converted to NV12 here);
	// only compressed modes go through Windows' decoder / converter.
	processed = !Uncompressed(chosen_sub);
	in_kind = chosen_sub == MFVideoFormat_YUY2 ? 1 : chosen_sub == MFVideoFormat_UYVY ? 2 : 0;
	IMFAttributes *attr = nullptr;
	hr = MFCreateAttributes(&attr, 3);
	if(SUCCEEDED(hr))
	{
		attr->SetUINT32(MF_LOW_LATENCY, TRUE);
		if(processed)
		{
			attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
			attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		}
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
		out->SetGUID(MF_MT_SUBTYPE, processed ? MFVideoFormat_NV12 : chosen_sub);
		hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
	}
	SafeRelease(out);
	if(FAILED(hr))
	{
		*error = "This capture card's picture format isn't supported (" + Format().subtype + ").";
		LOGE("Capture: output type not accepted: 0x%08lx", (unsigned long)hr);
		return false;
	}
	if(processed)
		in_kind = 0;
	return true;
}

// One frame from the card into a new NV12 AVFrame (limited or full range, BT.709 for HD).
AVFrame *CaptureSource::Convert(const uint8_t *data, long pitch, size_t len)
{
	const int w = out_w & ~1, h = out_h & ~1;
	const long row = in_kind == 0 ? w : w * 2;
	if(w <= 0 || h <= 0 || pitch < row)
		return nullptr;
	if(in_kind != 0 && len && len < (size_t)pitch * (h - 1) + (size_t)row)
		return nullptr;
	AVFrame *f = av_frame_alloc();
	if(!f)
		return nullptr;
	f->format = AV_PIX_FMT_NV12;
	f->width = w;
	f->height = h;
	if(av_frame_get_buffer(f, 64) < 0)
	{
		av_frame_free(&f);
		return nullptr;
	}
	if(in_kind == 0)
	{
		// NV12: Y rows then UV rows. A plain buffer may pad the Y plane to a multiple of 16 lines.
		int y_rows = out_h;
		if(len)
		{
			int rows = (int)(len / (size_t)pitch);
			int aligned = (out_h + 15) & ~15;
			if(rows != out_h * 3 / 2 && rows == aligned * 3 / 2)
				y_rows = aligned;
			if(len < (size_t)pitch * (y_rows + h / 2))
			{
				av_frame_free(&f);
				return nullptr;
			}
		}
		Nv12Copy(data, pitch, data + (size_t)pitch * y_rows, w, h, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
	}
	else
		Packed422ToNv12(data, pitch, w, h, in_kind == 2, f->data[0], f->linesize[0], f->data[1], f->linesize[1]);
	f->color_range = opt.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	bool hd = h >= 720;
	f->colorspace = hd ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
	f->color_primaries = hd ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
	f->color_trc = hd ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
	return f;
}

void CaptureSource::Thread()
{
	HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
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
			long natural = in_kind == 0 ? (long)w : (long)w * 2;
			stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)(INT32)st : (LONG)natural;
			if(stride < natural)
				stride = (LONG)natural;
			t->Release();
			LOGI("Capture output: %dx%d, stride %ld, %s", out_w, out_h, (long)stride,
				in_kind == 1 ? "YUY2 -> NV12" : in_kind == 2 ? "UYVY -> NV12" : processed ? "NV12 (converted by Windows)" : "NV12");
		}
	};
	if(open_ok)
		read_output_type();

	int bad_frames = 0;
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
		AVFrame *f = nullptr;
		IMFMediaBuffer *buf = nullptr;
		if(SUCCEEDED(sample->GetBufferByIndex(0, &buf)))
		{
			BYTE *data = nullptr;
			LONG pitch = 0;
			DWORD len = 0;
			buf->GetCurrentLength(&len);
			IMF2DBuffer *b2 = nullptr;
			bool locked2d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&data, &pitch));
			bool locked = false;
			if(!locked2d)
			{
				SafeRelease(b2);
				locked = SUCCEEDED(buf->Lock(&data, nullptr, &len));
				pitch = stride;
			}
			if((locked2d || locked) && data && pitch > 0)
				f = Convert(data, (long)pitch, locked2d ? 0 : (size_t)len);
			if(locked2d)
				b2->Unlock2D();
			else if(locked)
				buf->Unlock();
			SafeRelease(b2);
		}
		SafeRelease(buf);
		SafeRelease(sample);
		if(!f)
		{
			if(++bad_frames == 30)
				LOGW("Capture: frames could not be read (%dx%d, stride %ld)", out_w, out_h, (long)stride);
			continue;
		}
		FeedEncoder(f);
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
			format.width = f->width;
			format.height = f->height;
		}
		if(on_frame)
			on_frame(first);
		first = false;
		if(frame_event)
			SetEvent(frame_event);
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
// Recording: H.264 on the GPU's video encoder, handed to the recorder like the console's stream

void CaptureSource::FeedEncoder(const AVFrame *f)
{
	Recorder *r = recorder.load();
	if(!r || !(r->Recording() || r->ReplayEnabled()) || enc_failed)
		return;
	AVFrame *c = av_frame_clone(f);
	if(!c)
		return;
	{
		std::lock_guard<std::mutex> lock(enc_mutex);
		while(enc_queue.size() >= 4)
		{
			av_frame_free(&enc_queue.front());
			enc_queue.pop_front();
		}
		enc_queue.push_back(c);
	}
	enc_cv.notify_one();
}

bool CaptureSource::OpenEncoder(int w, int h, double rate)
{
	static const char *names[] = { "h264_nvenc", "h264_amf", "h264_qsv", "libx264" };
	int fps_i = std::max(1, (int)std::lround(rate > 1 ? rate : 60));
	for(const char *name : names)
	{
		const AVCodec *codec = avcodec_find_encoder_by_name(name);
		if(!codec)
			continue;
		AVCodecContext *c = avcodec_alloc_context3(codec);
		c->width = w;
		c->height = h;
		c->pix_fmt = AV_PIX_FMT_NV12;
		c->time_base = AVRational{ 1, 1000 };
		c->framerate = AVRational{ fps_i, 1 };
		c->gop_size = fps_i * 2;        // a keyframe every 2 s, so instant replay can start anywhere
		c->max_b_frames = 0;
		c->bit_rate = h > 1080 ? 60000000 : 30000000;
		c->rc_max_rate = c->bit_rate * 3 / 2;
		c->rc_buffer_size = (int)c->bit_rate;
		c->color_range = opt.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		c->colorspace = h >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
		c->color_primaries = h >= 720 ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
		c->color_trc = h >= 720 ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
		av_opt_set(c->priv_data, "forced-idr", "1", 0);
		if(!strcmp(name, "h264_nvenc"))
		{
			av_opt_set(c->priv_data, "preset", "p5", 0);
			av_opt_set(c->priv_data, "tune", "hq", 0);
			av_opt_set(c->priv_data, "rc", "vbr", 0);
		}
		else if(!strcmp(name, "libx264"))
		{
			av_opt_set(c->priv_data, "preset", "veryfast", 0);
			av_opt_set(c->priv_data, "crf", "18", 0);
		}
		if(avcodec_open2(c, codec, nullptr) == 0)
		{
			enc = c;
			enc_pkt = av_packet_alloc();
			enc_name = name;
			enc_pts = 0;
			LOGI("Capture recording encoder: %s", name);
			return true;
		}
		avcodec_free_context(&c);
	}
	LOGW("Capture: no H.264 encoder could be opened; recording is unavailable");
	return false;
}

void CaptureSource::CloseEncoder()
{
	if(enc)
		avcodec_free_context(&enc);
	if(enc_pkt)
		av_packet_free(&enc_pkt);
}

void CaptureSource::EncoderThread()
{
	uint64_t start_ms = 0;
	int enc_w = 0, enc_h = 0;
	for(;;)
	{
		AVFrame *f = nullptr;
		{
			std::unique_lock<std::mutex> lock(enc_mutex);
			enc_cv.wait(lock, [&] { return enc_quit || !enc_queue.empty(); });
			if(enc_quit)
				break;
			f = enc_queue.front();
			enc_queue.pop_front();
		}
		if(enc && (enc_w != f->width || enc_h != f->height))
			CloseEncoder();
		if(!enc && !enc_failed)
		{
			if(OpenEncoder(f->width, f->height, Format().fps))
			{
				enc_w = f->width;
				enc_h = f->height;
				start_ms = NowMs();
				force_key = true;
			}
			else
				enc_failed = true;
		}
		Recorder *r = recorder.load();
		if(enc && r)
		{
			int64_t pts = (int64_t)(NowMs() - start_ms);
			if(pts <= enc_pts && enc_pts > 0)
				pts = enc_pts + 1;
			enc_pts = pts;
			f->pts = pts;
			f->pict_type = force_key.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
			if(avcodec_send_frame(enc, f) == 0)
				while(avcodec_receive_packet(enc, enc_pkt) == 0)
				{
					r->OnVideoPacket(enc_pkt->data, (size_t)enc_pkt->size);
					av_packet_unref(enc_pkt);
				}
		}
		av_frame_free(&f);
	}
	CloseEncoder();
}

// ---------------------------------------------------------------------------
// Audio: the card's recording device straight to the game audio output

void SDLCALL CaptureSource::AudioCb(void *user, SDL_AudioStream *stream, int additional, int)
{
	auto *self = static_cast<CaptureSource *>(user);
	if(additional <= 0)
		return;
	self->audio_buf.resize((size_t)additional / sizeof(int16_t));
	int got = SDL_GetAudioStreamData(stream, self->audio_buf.data(), additional);
	if(got <= 0)
		return;
	size_t frames = (size_t)got / (sizeof(int16_t) * 2);
	if(self->audio)
		self->audio->PushGame(self->audio_buf.data(), frames);
	if(Recorder *r = self->recorder.load())
		r->OnAudio(self->audio_buf.data(), frames, 2, 48000);
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
