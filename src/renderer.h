// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <memory>
#include <string>

struct AVFrame;
class VpScaler;

// Minimal COM smart pointer (avoids depending on WRL).
template<class T>
class Com
{
public:
	Com() = default;
	Com(const Com &o) : p(o.p) { if(p) p->AddRef(); }
	Com &operator=(const Com &o) { if(o.p) o.p->AddRef(); Reset(); p = o.p; return *this; }
	~Com() { Reset(); }
	void Reset() { if(p) { p->Release(); p = nullptr; } }
	T *Get() const { return p; }
	T **Put() { Reset(); return &p; }
	T *operator->() const { return p; }
	explicit operator bool() const { return p != nullptr; }
	template<class U> HRESULT As(Com<U> *out) const { return p ? p->QueryInterface(__uuidof(U), (void **)out->Put()) : E_POINTER; }
private:
	T *p = nullptr;
};

enum class UpscaleMode : int { Off = 0, Enhanced = 1, AI = 2 };
enum class AspectMode : int { Fit = 0, Fill = 1, Stretch = 2 };

// How each decoded frame is processed (applied in SubmitFrame).
struct ProcessOptions
{
	UpscaleMode upscale = UpscaleMode::Off;
	int target_height = 0;          // 0 = match the video area on screen
	float artifact_reduction = 0.4f;
	float sharpness = 0.5f;         // enhanced upscaler
	float ai_sharpness = 0.35f;     // extra sharpening after AI super resolution
	float deband = 0.3f;            // gradient banding removal, 0 = off
	float paper_white = 203.0f;     // used to put SDR and HDR on one scale for ambient light
	bool ai_hdr = false;            // NVIDIA RTX Video HDR for SDR streams on an HDR display
	bool frame_gen = false;
	bool ambient = false;
	AspectMode aspect = AspectMode::Fit;
};

// How the picture is shown (applied in Present).
struct PresentOptions
{
	int sync_interval = 1;          // 0 = present immediately (tearing allowed if supported)
	float paper_white = 203.0f;
	float hdr_boost = 0.0f;         // SDR -> HDR highlight expansion (shader), 0..1
	float ambient = 0.0f;           // ambient light intensity, 0 = off
	AspectMode aspect = AspectMode::Fit;
	int ambient_style = 0;          // 0 glow, 1 extend
	float brightness = 0.0f, contrast = 1.0f, saturation = 1.0f, vibrance = 0.0f;
	bool compare = false;           // split screen: original on the left, processed on the right
	float compare_split = 0.5f;     // divider position, fraction of the video width
};

enum class VideoView { None, Latest, Interpolated };

// D3D11 renderer: hardware decoded frames in, HDR10 or SDR swap chain out.
class Renderer
{
public:
	Renderer();
	~Renderer();

	bool Init(HWND hwnd);
	void Shutdown();
	void Resize(int width, int height);

	ID3D11Device *Device() const { return device.Get(); }
	ID3D11DeviceContext *Context() const { return context.Get(); }

	void RefreshDisplayInfo();
	bool DisplayHdr() const { return display_hdr; }
	float DisplayPeakNits() const { return display_peak_nits; }
	int DisplayHz() const { return display_hz; }
	bool OutputHdr() const { return output_hdr; }

	// Video ------------------------------------------------------------------
	bool SubmitFrame(AVFrame *frame, const ProcessOptions &options);
	void ResetVideo();
	bool HasVideo() const { return frames_submitted > 0; }
	bool VideoHdr() const { return video_hdr; }
	int VideoWidth() const { return video_w; }
	int VideoHeight() const { return video_h; }
	int OutputVideoWidth() const;   // size of what's shown (after upscaling)
	int OutputVideoHeight() const;
	const std::string &UpscalerStatus() const { return upscaler_status; }
	bool AiSuperResolutionAvailable() const;
	bool AiHdrAvailable() const;
	std::string GpuVendorName() const;

	// Frame generation: motion is estimated once per new frame, then any number of
	// in-between frames can be built (t in 0..1, 0 = previous frame, 1 = current).
	bool PrepareMotion();
	bool BuildInterpolatedFrame(float t);

	// UI ---------------------------------------------------------------------
	void BeginOverlay();
	void Present(VideoView view, const PresentOptions &options);

	// Where the video is drawn in the window: x, y, w, h in pixels.
	void GetVideoRect(AspectMode aspect, float rect[4]) const { float uv[4]; VideoRect(aspect, width, height, rect, uv); }

	// Saves the current frame (SDR, tone mapped if needed) as a PNG.
	bool SaveScreenshot(const std::wstring &path, const PresentOptions &options);

private:
	struct Target
	{
		Com<ID3D11Texture2D> tex;
		Com<ID3D11RenderTargetView> rtv;
		Com<ID3D11ShaderResourceView> srv;
		Com<ID3D11UnorderedAccessView> uav;
		int w = 0, h = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		void Reset() { tex.Reset(); rtv.Reset(); srv.Reset(); uav.Reset(); w = h = 0; format = DXGI_FORMAT_UNKNOWN; }
	};

	bool CreateSwapChain();
	void CreateBackbufferTargets();
	void ReleaseBackbufferTargets();
	void ApplyOutputMode();
	bool CompileShaders();
	bool MakeTarget(Target &t, int w, int h, DXGI_FORMAT fmt, bool rtv, bool uav);
	bool EnsureTarget(Target &t, int w, int h, DXGI_FORMAT fmt, bool rtv, bool uav);
	bool EnsureVideoTargets(int w, int h);
	bool EnsureMotionTargets();
	void ConvertToRgb(ID3D11ShaderResourceView *const *planes, int plane_count, bool semi_planar, const float matrix[12], float uv_sx, float uv_sy);
	void DrawFullscreen(ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, int w, int h,
			ID3D11ShaderResourceView *const *srvs, int srv_count, ID3D11Buffer *cb);
	void Dispatch(ID3D11ComputeShader *cs, ID3D11ShaderResourceView *const *srvs, int srv_count,
			ID3D11UnorderedAccessView *uav, ID3D11Buffer *cb, int gx, int gy);
	void UpdateCB(ID3D11Buffer *cb, const void *data, size_t size);
	void ComputeLumaPyramid(int slot);
	void UpdateAmbient(float paper_white);
	void VideoRect(AspectMode aspect, int win_w, int win_h, float rect[4], float uv[4]) const;
	void ProcessFrame(AVFrame *frame, const ProcessOptions &o);
	bool ShaderUpscale(int tw, int th, const ProcessOptions &o);
	// deband + sharpen from `src` (tw x th) into proc[latest]; false if nothing to do
	bool PostProcess(ID3D11ShaderResourceView *src, int tw, int th, float deband, float deband_scale, float sharpness, float noise_floor);
	void Deband(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, int w, int h, float strength, float scale);
	void DrawVideoOut(ID3D11RenderTargetView *rtv, int w, int h, ID3D11ShaderResourceView *video, int vw, int vh,
			bool content_hdr, bool out_hdr, const PresentOptions &o, bool with_ambient, bool full_rect);
	ID3D11ShaderResourceView *DisplaySrv(int slot, int *w, int *h) const;

	HWND hwnd = nullptr;
	int width = 0, height = 0;

	Com<ID3D11Device> device;
	Com<ID3D11DeviceContext> context;
	Com<IDXGISwapChain1> swapchain;
	Com<IDXGISwapChain3> swapchain3;
	HANDLE frame_wait = nullptr;
	bool tearing_supported = false;
	UINT swap_flags = 0;
	std::unique_ptr<VpScaler> vp;

	bool display_hdr = false;
	float display_peak_nits = 1000.0f;
	int display_hz = 60;
	bool output_hdr = false;
	DXGI_FORMAT bb_format = DXGI_FORMAT_B8G8R8A8_UNORM;

	Com<ID3D11RenderTargetView> bb_rtv;
	Target overlay;

	// Shaders and states
	Com<ID3D11VertexShader> vs;
	Com<ID3D11PixelShader> ps_nv12, ps_planar, ps_denoise, ps_edge, ps_sharpen, ps_deband, ps_out, ps_overlay, ps_interp;
	Com<ID3D11ComputeShader> cs_luma_half, cs_down2, cs_search, cs_median, cs_box8, cs_amb_blend;
	Com<ID3D11Buffer> cb_yuv, cb_denoise, cb_up, cb_sharp, cb_deband, cb_out, cb_ui, cb_me, cb_interp, cb_amb, cb_amb_down;
	Com<ID3D11SamplerState> samp_linear;
	Com<ID3D11BlendState> blend_premul;
	Com<ID3D11RasterizerState> raster;

	// Decoder output copy (NV12/P010) or software planes
	Com<ID3D11Texture2D> hw_copy;
	DXGI_FORMAT hw_copy_format = DXGI_FORMAT_UNKNOWN;
	UINT hw_copy_w = 0, hw_copy_h = 0;
	Com<ID3D11ShaderResourceView> hw_srv_y, hw_srv_uv;
	Com<ID3D11Texture2D> sw_planes[3];
	Com<ID3D11ShaderResourceView> sw_srvs[3];
	int sw_format = -1, sw_w = 0, sw_h = 0;

	// Stream-resolution RGB history (RGBA16F, still gamma/PQ encoded)
	int video_w = 0, video_h = 0;
	bool video_hdr = false;
	uint64_t frames_submitted = 0;
	int latest = 0;
	Target rgb[2];

	// Processed (upscaled / AI) history
	Target proc[2];
	int slot_gen[2] = { -1, -1 };   // processing generation of each slot, -1 = not processed
	int proc_gen = 0;
	bool proc_hdr = false;          // processed output is PQ/BT.2020
	int last_tw = 0, last_th = 0, last_mode = -1;
	bool last_ai_hdr = false;
	Target clean, clean2, up_tmp, vp_out, post_tmp;
	DXGI_FORMAT vp_format = DXGI_FORMAT_R10G10B10A2_UNORM;
	float paper_white_proc = 203.0f;
	std::string upscaler_status = "Off";

	// Ambient light
	Target amb_small1, amb_small2, amb[2];
	int amb_index = 0;
	bool amb_valid = false;

	// Frame generation
	bool frame_gen_enabled = false;
	bool motion_ready = false;
	int grid_w = 0, grid_h = 0, fine_w = 0, fine_h = 0;
	Target luma_half[2], luma_quarter[2];
	bool luma_valid[2] = {};
	Target mv_coarse[2], mv_fine[2], mv_final[2];
	int final_idx = 0;
	bool final_valid = false;
	Target interp;
	bool interp_ready = false;
};
