// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <cstdint>

struct AVFrame;

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

struct RenderOptions
{
	bool upscale = false;      // Lanczos scaling (+ sharpening) instead of bilinear
	float sharpness = 0.5f;
	float paper_white = 203.0f;
	int sync_interval = 1;     // 0 = present immediately (tearing allowed if supported)
};

enum class VideoView { None, Latest, Interpolated };

// D3D11 renderer: hardware decoded frames in, HDR10 or SDR swap chain out.
class Renderer
{
public:
	bool Init(HWND hwnd);
	void Shutdown();
	void Resize(int width, int height);

	ID3D11Device *Device() const { return device.Get(); }
	ID3D11DeviceContext *Context() const { return context.Get(); }

	// Monitor capabilities of the display the window is on.
	void RefreshDisplayInfo();
	bool DisplayHdr() const { return display_hdr; }
	float DisplayPeakNits() const { return display_peak_nits; }
	int DisplayHz() const { return display_hz; }
	bool OutputHdr() const { return output_hdr; }

	// Video ------------------------------------------------------------------
	// Takes a decoded frame (D3D11 hardware frame or software YUV) and converts it to RGB.
	bool SubmitFrame(AVFrame *frame);
	void ResetVideo();
	bool HasVideo() const { return frames_submitted > 0; }
	bool VideoHdr() const { return video_hdr; }
	int VideoWidth() const { return video_w; }
	int VideoHeight() const { return video_h; }

	// Frame generation: builds the frame halfway between the last two submitted frames.
	void SetFrameGenEnabled(bool enabled) { frame_gen_enabled = enabled; }
	bool BuildInterpolatedFrame();

	// UI ---------------------------------------------------------------------
	// Binds the UI overlay target; render ImGui after this, then call Present.
	void BeginOverlay();

	void Present(VideoView view, const RenderOptions &options);

private:
	bool CreateSwapChain();
	void CreateBackbufferTargets();
	void ReleaseBackbufferTargets();
	void ApplyOutputMode();
	bool CompileShaders();
	bool EnsureVideoTargets(int w, int h);
	bool EnsureInterpTargets();
	void ConvertToRgb(ID3D11ShaderResourceView *const *planes, int plane_count, bool semi_planar, const float matrix[12], float uv_sx, float uv_sy);
	void DrawFullscreen(ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, int w, int h,
			ID3D11ShaderResourceView *const *srvs, int srv_count, ID3D11Buffer *cb);
	void Dispatch(ID3D11ComputeShader *cs, ID3D11ShaderResourceView *const *srvs, int srv_count,
			ID3D11UnorderedAccessView *uav, ID3D11Buffer *cb, int gx, int gy);
	void UpdateCB(ID3D11Buffer *cb, const void *data, size_t size);
	void ComputeLumaPyramid(int slot);

	HWND hwnd = nullptr;
	int width = 0, height = 0;

	Com<ID3D11Device> device;
	Com<ID3D11DeviceContext> context;
	Com<IDXGISwapChain1> swapchain;
	Com<IDXGISwapChain3> swapchain3;
	HANDLE frame_wait = nullptr;
	bool tearing_supported = false;
	UINT swap_flags = 0;

	bool display_hdr = false;
	float display_peak_nits = 1000.0f;
	int display_hz = 60;
	bool output_hdr = false;
	DXGI_FORMAT bb_format = DXGI_FORMAT_B8G8R8A8_UNORM;

	Com<ID3D11RenderTargetView> bb_rtv;
	Com<ID3D11Texture2D> overlay_tex;
	Com<ID3D11RenderTargetView> overlay_rtv;
	Com<ID3D11ShaderResourceView> overlay_srv;

	// Shaders and states
	Com<ID3D11VertexShader> vs;
	Com<ID3D11PixelShader> ps_nv12, ps_planar, ps_cas, ps_out, ps_overlay, ps_interp;
	Com<ID3D11ComputeShader> cs_luma_half, cs_down2, cs_search, cs_median;
	Com<ID3D11Buffer> cb_yuv, cb_cas, cb_out, cb_ui, cb_me, cb_interp;
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

	// RGB history (stream resolution, RGBA16F, still gamma/PQ encoded)
	int video_w = 0, video_h = 0;
	bool video_hdr = false;
	uint64_t frames_submitted = 0;
	int latest = 0;
	Com<ID3D11Texture2D> rgb_tex[2];
	Com<ID3D11RenderTargetView> rgb_rtv[2];
	Com<ID3D11ShaderResourceView> rgb_srv[2];
	Com<ID3D11Texture2D> cas_tex;
	Com<ID3D11RenderTargetView> cas_rtv;
	Com<ID3D11ShaderResourceView> cas_srv;

	// Frame generation resources
	bool frame_gen_enabled = false;
	bool interp_ready = false;
	int grid_w = 0, grid_h = 0;
	Com<ID3D11Texture2D> luma_half_tex[2], luma_quarter_tex[2];
	Com<ID3D11ShaderResourceView> luma_half_srv[2], luma_quarter_srv[2];
	Com<ID3D11UnorderedAccessView> luma_half_uav[2], luma_quarter_uav[2];
	bool luma_valid[2] = {};
	Com<ID3D11Texture2D> mv_tex[2];
	Com<ID3D11ShaderResourceView> mv_srv[2];
	Com<ID3D11UnorderedAccessView> mv_uav[2];
	Com<ID3D11Texture2D> interp_tex;
	Com<ID3D11RenderTargetView> interp_rtv;
	Com<ID3D11ShaderResourceView> interp_srv;
};
