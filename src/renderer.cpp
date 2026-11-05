// SPDX-License-Identifier: AGPL-3.0-only
#include "renderer.h"
#include "log.h"
#include "shaders.h"
#include "vpscaler.h"

#include <d3dcompiler.h>
#include <wincodec.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Constant buffer layouts (16-byte aligned, must match shaders.h)

struct YuvCB { float rows[12]; float uv_scale[2]; float pad[2]; };
struct DenoiseCB { float strength; float pad[3]; };
struct UpCB { float src_size[2]; float edge_strength; float pad; };
struct SharpCB { float sharpness; float noise_floor; float texture_boost; float clarity; float overshoot; float pad[3]; };
struct DebandCB { float threshold; float radius; float seed; float grain; };
struct AmbDownCB { int decode; float paper_white; float pad[2]; };
struct OutCB
{
	float vid_rect[4];
	float uv_rect[4];
	float src_size[2]; int scaler; int mode;
	float paper_white; float peak; float ambient; float hdr_boost;
	float brightness; float contrast; float saturation; float vibrance;
	float win_size[2]; float split; int orig_mode;
	int ambient_style; float pad[3];
};
struct UiCB { int hdr_out; float ui_nits; float pad[2]; };
struct MeCB
{
	int grid[2]; int block; int radius;
	int level_scale; float lambda; int use_pred; int use_temporal;
	float pred_scale[2]; float temporal_scale[2];
};
struct InterpCB { float src_size[2]; float grid_scale[2]; float t; float pad[3]; };
struct AmbCB { float alpha; float pad[3]; };

static const int kCoarseBlockPx = 16; // coarse motion field: one vector per 16x16 pixels
static const int kFineBlockPx = 8;    // final motion field: one vector per 8x8 pixels
static const DXGI_FORMAT kProcFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

Renderer::Renderer() : vp(new VpScaler()) {}
Renderer::~Renderer() = default;

// ---------------------------------------------------------------------------
// Init / shutdown

bool Renderer::Init(HWND window)
{
	hwnd = window;
	RECT rc;
	GetClientRect(hwnd, &rc);
	width = std::max(1, (int)(rc.right - rc.left));
	height = std::max(1, (int)(rc.bottom - rc.top));

	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
			D3D11_SDK_VERSION, device.Put(), nullptr, context.Put());
	if(FAILED(hr))
	{
		LOGE("D3D11CreateDevice failed (0x%08lx)", (unsigned long)hr);
		return false;
	}

	// The decoder (FFmpeg D3D11VA) and the renderer share the immediate context from different threads.
	Com<ID3D10Multithread> mt;
	if(SUCCEEDED(context.As(&mt)))
		mt->SetMultithreadProtected(TRUE);

	vp->Init(device.Get(), context.Get());

	if(!CompileShaders())
		return false;

	auto make_cb = [&](size_t size, Com<ID3D11Buffer> &out) {
		D3D11_BUFFER_DESC d = {};
		d.ByteWidth = (UINT)((size + 15) & ~15);
		d.Usage = D3D11_USAGE_DYNAMIC;
		d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		device->CreateBuffer(&d, nullptr, out.Put());
	};
	make_cb(sizeof(YuvCB), cb_yuv);
	make_cb(sizeof(DenoiseCB), cb_denoise);
	make_cb(sizeof(UpCB), cb_up);
	make_cb(sizeof(SharpCB), cb_sharp);
	make_cb(sizeof(DebandCB), cb_deband);
	make_cb(sizeof(AmbDownCB), cb_amb_down);
	make_cb(sizeof(OutCB), cb_out);
	make_cb(sizeof(UiCB), cb_ui);
	make_cb(sizeof(MeCB), cb_me);
	make_cb(sizeof(InterpCB), cb_interp);
	make_cb(sizeof(AmbCB), cb_amb);

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	device->CreateSamplerState(&sd, samp_linear.Put());

	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].BlendEnable = TRUE;
	bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	device->CreateBlendState(&bd, blend_premul.Put());

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	device->CreateRasterizerState(&rd, raster.Put());

	// repeated-frame detector counters (GPU) + readback buffer
	{
		D3D11_BUFFER_DESC bd2 = {};
		bd2.ByteWidth = 16;
		bd2.Usage = D3D11_USAGE_DEFAULT;
		bd2.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd2.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		device->CreateBuffer(&bd2, nullptr, diff_buf.Put());
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
		ud.Format = DXGI_FORMAT_R32_TYPELESS;
		ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		ud.Buffer.NumElements = 4;
		ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if(diff_buf)
			device->CreateUnorderedAccessView(diff_buf.Get(), &ud, diff_uav.Put());
		bd2.Usage = D3D11_USAGE_STAGING;
		bd2.BindFlags = 0;
		bd2.MiscFlags = 0;
		bd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for(int i = 0; i < kSlots; i++)
			device->CreateBuffer(&bd2, nullptr, diff_staging[i].Put());
	}

	if(!CreateSwapChain())
		return false;
	RefreshDisplayInfo();
	return true;
}

void Renderer::Shutdown()
{
	if(context)
	{
		context->ClearState();
		context->Flush();
	}
	if(frame_wait)
	{
		CloseHandle(frame_wait);
		frame_wait = nullptr;
	}
}

bool Renderer::CompileShaders()
{
	auto compile = [&](const std::string &src, const char *profile, Com<ID3DBlob> &blob) -> bool {
		Com<ID3DBlob> errors;
		HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.Put(), errors.Put());
		if(FAILED(hr))
		{
			LOGE("Shader compile error (%s): %s", profile, errors ? (const char *)errors->GetBufferPointer() : "unknown");
			return false;
		}
		return true;
	};
	auto ps = [&](const std::string &src, Com<ID3D11PixelShader> &out) -> bool {
		Com<ID3DBlob> blob;
		if(!compile(std::string(shaders::kCommon) + src, "ps_5_0", blob))
			return false;
		return SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out.Put()));
	};
	auto cs = [&](const std::string &src, Com<ID3D11ComputeShader> &out) -> bool {
		Com<ID3DBlob> blob;
		if(!compile(std::string(shaders::kCommon) + src, "cs_5_0", blob))
			return false;
		return SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out.Put()));
	};

	Com<ID3DBlob> vs_blob;
	if(!compile(std::string(shaders::kCommon) + shaders::kVS, "vs_5_0", vs_blob)
		|| FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, vs.Put())))
		return false;

	bool ok = ps(std::string(shaders::kYuvCB) + shaders::kPsYuvNv12, ps_nv12)
		&& ps(std::string(shaders::kYuvCB) + shaders::kPsYuvPlanar, ps_planar)
		&& ps(shaders::kPsDenoise, ps_denoise)
		&& ps(shaders::kPsEdgeUpscale, ps_edge)
		&& ps(shaders::kPsSharpen, ps_sharpen)
		&& ps(shaders::kPsDeband, ps_deband)
		&& ps(shaders::kPsVideoOut, ps_out)
		&& ps(shaders::kPsOverlay, ps_overlay)
		&& ps(shaders::kPsInterp, ps_interp)
		&& cs(shaders::kCsLumaHalf, cs_luma_half)
		&& cs(shaders::kCsDown2, cs_down2)
		&& cs(std::string(shaders::kMeCB) + shaders::kCsMotionSearch, cs_search)
		&& cs(shaders::kCsMedian, cs_median)
		&& cs(shaders::kCsBox8, cs_box8)
		&& cs(shaders::kCsAmbientBlend, cs_amb_blend)
		&& cs(shaders::kCsFrameDiff, cs_frame_diff);
	if(!ok)
		LOGE("Failed to build shaders");
	return ok;
}

// ---------------------------------------------------------------------------
// Swap chain / display

bool Renderer::CreateSwapChain()
{
	Com<IDXGIDevice> dxgi_device;
	Com<IDXGIAdapter> adapter;
	Com<IDXGIFactory2> factory;
	if(FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(adapter.Put()))
		|| FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), (void **)factory.Put())))
	{
		LOGE("Could not get the DXGI factory");
		return false;
	}

	Com<IDXGIFactory5> factory5;
	if(SUCCEEDED(factory.As(&factory5)))
	{
		BOOL allow = FALSE;
		if(SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
			tearing_supported = allow != FALSE;
	}

	swap_flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	if(tearing_supported)
		swap_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.Format = bb_format;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 3;
	desc.Scaling = DXGI_SCALING_STRETCH;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.Flags = swap_flags;

	HRESULT hr = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr, nullptr, swapchain.Put());
	if(FAILED(hr))
	{
		LOGE("CreateSwapChainForHwnd failed (0x%08lx)", (unsigned long)hr);
		return false;
	}
	factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
	swapchain.As(&swapchain3);

	Com<IDXGISwapChain2> sc2;
	if(SUCCEEDED(swapchain.As(&sc2)))
	{
		sc2->SetMaximumFrameLatency(1);
		frame_wait = sc2->GetFrameLatencyWaitableObject();
	}
	CreateBackbufferTargets();
	return true;
}

void Renderer::ReleaseBackbufferTargets()
{
	if(context)
		context->OMSetRenderTargets(0, nullptr, nullptr);
	bb_rtv.Reset();
	overlay.Reset();
}

void Renderer::CreateBackbufferTargets()
{
	Com<ID3D11Texture2D> bb;
	swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)bb.Put());
	device->CreateRenderTargetView(bb.Get(), nullptr, bb_rtv.Put());
	MakeTarget(overlay, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, true, false);
}

void Renderer::Resize(int w, int h)
{
	w = std::max(1, w);
	h = std::max(1, h);
	if(w == width && h == height)
		return;
	width = w;
	height = h;
	ReleaseBackbufferTargets();
	HRESULT hr = swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swap_flags);
	if(FAILED(hr))
		LOGE("ResizeBuffers failed (0x%08lx)", (unsigned long)hr);
	CreateBackbufferTargets();
	ApplyOutputMode();
}

void Renderer::RefreshDisplayInfo()
{
	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if(GetMonitorInfoW(monitor, &mi))
	{
		DEVMODEW dm = {};
		dm.dmSize = sizeof(dm);
		if(EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
			display_hz = (int)dm.dmDisplayFrequency;
	}

	// A fresh factory is needed to see changes to the Windows HDR toggle.
	bool hdr = false;
	float peak = 1000.0f;
	Com<IDXGIFactory1> factory;
	if(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)factory.Put())))
	{
		Com<IDXGIAdapter1> adapter;
		for(UINT a = 0; factory->EnumAdapters1(a, adapter.Put()) != DXGI_ERROR_NOT_FOUND; a++)
		{
			Com<IDXGIOutput> output;
			for(UINT o = 0; adapter->EnumOutputs(o, output.Put()) != DXGI_ERROR_NOT_FOUND; o++)
			{
				DXGI_OUTPUT_DESC od;
				if(FAILED(output->GetDesc(&od)) || od.Monitor != monitor)
					continue;
				Com<IDXGIOutput6> output6;
				DXGI_OUTPUT_DESC1 od1;
				if(SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&od1)))
				{
					hdr = od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
					if(od1.MaxLuminance > 100.0f)
						peak = od1.MaxLuminance;
				}
			}
		}
	}

	if(hdr != display_hdr)
		LOGI("Display HDR is %s", hdr ? "on" : "off");
	display_hdr = hdr;
	display_peak_nits = peak;

	if(output_hdr != display_hdr)
	{
		output_hdr = display_hdr;
		ReleaseBackbufferTargets();
		bb_format = output_hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
		HRESULT hr = swapchain->ResizeBuffers(0, width, height, bb_format, swap_flags);
		if(FAILED(hr))
			LOGE("Switching swap chain format failed (0x%08lx)", (unsigned long)hr);
		CreateBackbufferTargets();
		ApplyOutputMode();
	}
}

void Renderer::ApplyOutputMode()
{
	if(!swapchain3)
		return;
	DXGI_COLOR_SPACE_TYPE cs = output_hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
	UINT support = 0;
	if(SUCCEEDED(swapchain3->CheckColorSpaceSupport(cs, &support)) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
		swapchain3->SetColorSpace1(cs);
	else if(output_hdr)
		LOGW("Swap chain does not accept the HDR10 color space");
}

bool Renderer::AiSuperResolutionAvailable() const { return vp->SuperResolutionSupported(); }
bool Renderer::AiHdrAvailable() const { return vp->AiHdrSupported(); }

std::string Renderer::GpuVendorName() const
{
	switch(vp->GetVendor())
	{
		case VpScaler::Vendor::Nvidia: return "NVIDIA";
		case VpScaler::Vendor::Intel: return "Intel";
		case VpScaler::Vendor::Amd: return "AMD";
		default: return "Unknown";
	}
}

// ---------------------------------------------------------------------------
// Helpers

void Renderer::UpdateCB(ID3D11Buffer *cb, const void *data, size_t size)
{
	D3D11_MAPPED_SUBRESOURCE m;
	if(SUCCEEDED(context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
	{
		memcpy(m.pData, data, size);
		context->Unmap(cb, 0);
	}
}

void Renderer::DrawFullscreen(ID3D11PixelShader *ps, ID3D11RenderTargetView *rtv, int w, int h,
		ID3D11ShaderResourceView *const *srvs, int srv_count, ID3D11Buffer *cb)
{
	D3D11_VIEWPORT vpt = { 0, 0, (float)w, (float)h, 0, 1 };
	context->RSSetViewports(1, &vpt);
	context->RSSetState(raster.Get());
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetInputLayout(nullptr);
	context->VSSetShader(vs.Get(), nullptr, 0);
	context->PSSetShader(ps, nullptr, 0);
	ID3D11SamplerState *s = samp_linear.Get();
	context->PSSetSamplers(0, 1, &s);
	context->PSSetShaderResources(0, srv_count, srvs);
	context->PSSetConstantBuffers(0, 1, &cb);
	context->Draw(3, 0);
	ID3D11ShaderResourceView *nulls[4] = {};
	context->PSSetShaderResources(0, 4, nulls);
	context->OMSetRenderTargets(0, nullptr, nullptr);
}

void Renderer::Dispatch(ID3D11ComputeShader *cs, ID3D11ShaderResourceView *const *srvs, int srv_count,
		ID3D11UnorderedAccessView *uav, ID3D11Buffer *cb, int gx, int gy)
{
	context->CSSetShader(cs, nullptr, 0);
	context->CSSetShaderResources(0, srv_count, srvs);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	if(cb)
		context->CSSetConstantBuffers(0, 1, &cb);
	context->Dispatch((gx + 7) / 8, (gy + 7) / 8, 1);
	ID3D11ShaderResourceView *nulls[4] = {};
	ID3D11UnorderedAccessView *null_uav = nullptr;
	context->CSSetShaderResources(0, 4, nulls);
	context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

bool Renderer::MakeTarget(Target &t, int w, int h, DXGI_FORMAT fmt, bool rtv, bool uav)
{
	t.Reset();
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv ? D3D11_BIND_RENDER_TARGET : 0) | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
	if(FAILED(device->CreateTexture2D(&td, nullptr, t.tex.Put()))
		|| (rtv && FAILED(device->CreateRenderTargetView(t.tex.Get(), nullptr, t.rtv.Put())))
		|| (uav && FAILED(device->CreateUnorderedAccessView(t.tex.Get(), nullptr, t.uav.Put())))
		|| FAILED(device->CreateShaderResourceView(t.tex.Get(), nullptr, t.srv.Put())))
	{
		LOGE("Could not create a %dx%d render target (format %d)", w, h, (int)fmt);
		t.Reset();
		return false;
	}
	t.w = w;
	t.h = h;
	t.format = fmt;
	return true;
}

bool Renderer::EnsureTarget(Target &t, int w, int h, DXGI_FORMAT fmt, bool rtv, bool uav)
{
	if(t.tex && t.w == w && t.h == h && t.format == fmt)
		return true;
	return MakeTarget(t, w, h, fmt, rtv, uav);
}

// YUV -> RGB matrix for normalized texture values.
//   bits: 8 or 10; msb_aligned: 10-bit values stored in the top bits of 16 (P010)
static void YuvMatrix(bool bt2020, bool full_range, int bits, bool msb_aligned, float out[12])
{
	double kr = bt2020 ? 0.2627 : 0.2126;
	double kb = bt2020 ? 0.0593 : 0.0722;
	double kg = 1.0 - kr - kb;

	double scale;
	if(bits == 8)
		scale = 255.0;
	else
		scale = msb_aligned ? 65535.0 / 64.0 : 65535.0;

	double sh = (double)(1 << (bits - 8));
	double yoff, yrange, coff, crange;
	if(full_range)
	{
		yoff = 0.0; yrange = (1 << bits) - 1;
		coff = (double)(1 << (bits - 1)); crange = (1 << bits) - 1;
	}
	else
	{
		yoff = 16.0 * sh; yrange = 219.0 * sh;
		coff = 128.0 * sh; crange = 224.0 * sh;
	}

	double ya = scale / yrange, yb = -yoff / yrange;
	double ca = scale / crange, cb = -coff / crange;

	double r_cr = 2.0 * (1.0 - kr);
	double b_cb = 2.0 * (1.0 - kb);
	double g_cb = -2.0 * kb * (1.0 - kb) / kg;
	double g_cr = -2.0 * kr * (1.0 - kr) / kg;

	out[0] = (float)ya; out[1] = 0.0f;               out[2] = (float)(r_cr * ca);  out[3] = (float)(yb + r_cr * cb);
	out[4] = (float)ya; out[5] = (float)(g_cb * ca); out[6] = (float)(g_cr * ca);  out[7] = (float)(yb + (g_cb + g_cr) * cb);
	out[8] = (float)ya; out[9] = (float)(b_cb * ca); out[10] = 0.0f;               out[11] = (float)(yb + b_cb * cb);
}

void Renderer::VideoRect(AspectMode aspect, int win_w, int win_h, float rect[4], float uv[4]) const
{
	float va = (video_w > 0 && video_h > 0) ? (float)video_w / (float)video_h : 16.0f / 9.0f;
	float wa = (float)win_w / (float)win_h;
	uv[0] = 0; uv[1] = 0; uv[2] = 1; uv[3] = 1;
	if(aspect == AspectMode::Stretch)
	{
		rect[0] = 0; rect[1] = 0; rect[2] = (float)win_w; rect[3] = (float)win_h;
		return;
	}
	if(aspect == AspectMode::Fill)
	{
		rect[0] = 0; rect[1] = 0; rect[2] = (float)win_w; rect[3] = (float)win_h;
		if(wa > va)
		{
			float span = va / wa; // crop top/bottom
			uv[1] = 0.5f - span * 0.5f; uv[3] = 0.5f + span * 0.5f;
		}
		else
		{
			float span = wa / va; // crop left/right
			uv[0] = 0.5f - span * 0.5f; uv[2] = 0.5f + span * 0.5f;
		}
		return;
	}
	float vw = (float)win_w, vh = vw / va;
	if(vh > win_h)
	{
		vh = (float)win_h;
		vw = vh * va;
	}
	rect[0] = std::floor((win_w - vw) * 0.5f);
	rect[1] = std::floor((win_h - vh) * 0.5f);
	rect[2] = std::floor(vw);
	rect[3] = std::floor(vh);
}

// ---------------------------------------------------------------------------
// Video

void Renderer::ResetVideo()
{
	frames_submitted = 0;
	interp_ready = false;
	amb_valid = false;
	for(int i = 0; i < kSlots; i++)
	{
		luma_valid[i] = false;
		slot_gen[i] = -1;
		slot_serial[i] = 0;
	}
	hist[0] = hist[1] = hist[2] = -1;
	latest = work = 0;
	for(auto &f : mvc)
		f.a = f.b = 0;
	last_mode = -1;
	upscaler_status = "Off";
	vp->Reset(); // decoder textures are recreated per session
}

bool Renderer::EnsureVideoTargets(int w, int h)
{
	if(w == video_w && h == video_h && rgb[0].tex)
		return true;
	video_w = w;
	video_h = h;
	ResetVideo();
	for(int i = 0; i < kSlots; i++)
		if(!MakeTarget(rgb[i], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false))
			return false;
	// Everything sized from the stream is rebuilt lazily
	mv_coarse[0].Reset();
	interp.Reset();
	LOGI("Video size %dx%d", w, h);
	return true;
}

int Renderer::OutputVideoWidth() const
{
	return (slot_gen[latest] >= 0 && proc[latest].tex) ? proc[latest].w : video_w;
}

int Renderer::OutputVideoHeight() const
{
	return (slot_gen[latest] >= 0 && proc[latest].tex) ? proc[latest].h : video_h;
}

ID3D11ShaderResourceView *Renderer::DisplaySrv(int slot, int *w, int *h) const
{
	if(slot_gen[slot] >= 0 && proc[slot].tex)
	{
		*w = proc[slot].w;
		*h = proc[slot].h;
		return proc[slot].srv.Get();
	}
	*w = video_w;
	*h = video_h;
	return rgb[slot].srv.Get();
}

void Renderer::ConvertToRgb(ID3D11ShaderResourceView *const *planes, int plane_count, bool semi_planar,
		const float matrix[12], float uv_sx, float uv_sy)
{
	YuvCB cb = {};
	memcpy(cb.rows, matrix, sizeof(cb.rows));
	cb.uv_scale[0] = uv_sx;
	cb.uv_scale[1] = uv_sy;
	UpdateCB(cb_yuv.Get(), &cb, sizeof(cb));
	DrawFullscreen(semi_planar ? ps_nv12.Get() : ps_planar.Get(), rgb[work].rtv.Get(), video_w, video_h,
			planes, plane_count, cb_yuv.Get());
}

int Renderer::FindSlot(uint64_t serial) const
{
	if(serial == 0)
		return -1;
	for(int i = 0; i < kSlots; i++)
		if(slot_serial[i] == serial)
			return i;
	return -1;
}

int Renderer::PickWorkSlot() const
{
	for(int i = 0; i < kSlots; i++)
		if(i != hist[0] && i != hist[1] && i != hist[2])
			return i;
	return 0;
}

void Renderer::IssueFrameDiff(int a, int b)
{
	diff_pending[a] = false;
	if(!diff_uav || !diff_staging[a] || !luma_valid[a] || !luma_valid[b])
		return;
	const UINT zeros[4] = {};
	context->ClearUnorderedAccessViewUint(diff_uav.Get(), zeros);
	int qw = luma_quarter[a].w, qh = luma_quarter[a].h;
	int gw = (qw + 3) / 4, gh = (qh + 3) / 4;
	ID3D11ShaderResourceView *srvs[2] = { luma_quarter[a].srv.Get(), luma_quarter[b].srv.Get() };
	Dispatch(cs_frame_diff.Get(), srvs, 2, diff_uav.Get(), nullptr, gw, gh);
	// copy into this slot's readback buffer; read back later without waiting for the GPU
	context->CopyResource(diff_staging[a].Get(), diff_buf.Get());
	diff_pending[a] = true;
	diff_blocks[a] = (UINT)(gw * gh);
}

int Renderer::PollFrameDiff(uint64_t serial, bool *duplicate, bool *cut)
{
	int slot = FindSlot(serial);
	if(slot < 0 || !diff_pending[slot])
		return -1;
	D3D11_MAPPED_SUBRESOURCE m;
	HRESULT hr = context->Map(diff_staging[slot].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
	if(hr == DXGI_ERROR_WAS_STILL_DRAWING)
		return 0;
	if(FAILED(hr))
	{
		diff_pending[slot] = false;
		return -1;
	}
	const UINT *c = (const UINT *)m.pData;
	UINT changed = c[0], big = c[1];
	context->Unmap(diff_staging[slot].Get(), 0);
	diff_pending[slot] = false;
	*duplicate = changed == 0;
	*cut = big > (UINT)(diff_blocks[slot] * 0.55);
	return 1;
}

void Renderer::DiscardFrame(uint64_t serial)
{
	int slot = FindSlot(serial);
	if(slot < 0)
		return;
	int n = 0;
	int keep[3] = { -1, -1, -1 };
	for(int i = 0; i < 3; i++)
		if(hist[i] >= 0 && hist[i] != slot)
			keep[n++] = hist[i];
	for(int i = 0; i < 3; i++)
		hist[i] = keep[i];
	slot_serial[slot] = 0;
	diff_pending[slot] = false;
	for(auto &f : mvc)
		if(f.a == serial || f.b == serial)
			f.a = f.b = 0;
	if(latest == slot && hist[0] >= 0)
		latest = hist[0];
}

SubmitResult Renderer::SubmitFrame(AVFrame *frame, const ProcessOptions &o)
{
	SubmitResult result;
	if(!frame || frame->width <= 0 || frame->height <= 0)
		return result;
	if(!EnsureVideoTargets(frame->width, frame->height))
		return result;
	work = PickWorkSlot();

	bool hdr = frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
	bool bt2020 = frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL
		|| (frame->colorspace == AVCOL_SPC_UNSPECIFIED && hdr);
	bool full_range = frame->color_range == AVCOL_RANGE_JPEG;
	if(hdr != video_hdr)
		LOGI("Stream is %s", hdr ? "HDR10 (PQ, BT.2020)" : "SDR (BT.709)");
	video_hdr = hdr;

	float matrix[12];

	if(frame->format == AV_PIX_FMT_D3D11)
	{
		auto *tex = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
		UINT index = (UINT)(intptr_t)frame->data[1];
		D3D11_TEXTURE2D_DESC sd;
		tex->GetDesc(&sd);
		if(!hw_copy || hw_copy_format != sd.Format || hw_copy_w != sd.Width || hw_copy_h != sd.Height)
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = sd.Width;
			td.Height = sd.Height;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = sd.Format;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			hw_srv_y.Reset();
			hw_srv_uv.Reset();
			if(FAILED(device->CreateTexture2D(&td, nullptr, hw_copy.Put())))
			{
				LOGE("Could not create decoder copy texture (format %d)", (int)sd.Format);
				return result;
			}
			bool p010 = sd.Format == DXGI_FORMAT_P010 || sd.Format == DXGI_FORMAT_P016;
			D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
			vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			vd.Texture2D.MipLevels = 1;
			vd.Format = p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
			device->CreateShaderResourceView(hw_copy.Get(), &vd, hw_srv_y.Put());
			vd.Format = p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
			device->CreateShaderResourceView(hw_copy.Get(), &vd, hw_srv_uv.Put());
			hw_copy_format = sd.Format;
			hw_copy_w = sd.Width;
			hw_copy_h = sd.Height;
			LOGI("Hardware decoding to %s textures", p010 ? "P010 (10-bit)" : "NV12 (8-bit)");
		}
		context->CopySubresourceRegion(hw_copy.Get(), 0, 0, 0, 0, tex, index, nullptr);
		bool p010 = hw_copy_format == DXGI_FORMAT_P010 || hw_copy_format == DXGI_FORMAT_P016;
		YuvMatrix(bt2020, full_range, p010 ? 10 : 8, true, matrix);
		ID3D11ShaderResourceView *srvs[2] = { hw_srv_y.Get(), hw_srv_uv.Get() };
		ConvertToRgb(srvs, 2, true, matrix, (float)frame->width / hw_copy_w, (float)frame->height / hw_copy_h);
	}
	else
	{
		int fmt = frame->format;
		bool semi = fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010LE;
		bool ten = fmt == AV_PIX_FMT_YUV420P10LE || fmt == AV_PIX_FMT_P010LE;
		bool planar = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUV420P10LE;
		if(!semi && !planar)
		{
			LOGE("Unsupported decoder output format %s", av_get_pix_fmt_name((AVPixelFormat)fmt));
			return result;
		}
		if(fmt == AV_PIX_FMT_YUVJ420P)
			full_range = true;
		int planes = semi ? 2 : 3;
		if(sw_format != fmt || sw_w != frame->width || sw_h != frame->height)
		{
			for(int p = 0; p < planes; p++)
			{
				D3D11_TEXTURE2D_DESC td = {};
				td.Width = p == 0 ? frame->width : (frame->width + 1) / 2;
				td.Height = p == 0 ? frame->height : (frame->height + 1) / 2;
				td.MipLevels = 1;
				td.ArraySize = 1;
				if(p == 0 || !semi)
					td.Format = ten ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
				else
					td.Format = ten ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
				td.SampleDesc.Count = 1;
				td.Usage = D3D11_USAGE_DYNAMIC;
				td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if(FAILED(device->CreateTexture2D(&td, nullptr, sw_planes[p].Put()))
					|| FAILED(device->CreateShaderResourceView(sw_planes[p].Get(), nullptr, sw_srvs[p].Put())))
				{
					LOGE("Could not create software frame textures");
					sw_format = -1;
					return result;
				}
			}
			sw_format = fmt;
			sw_w = frame->width;
			sw_h = frame->height;
			LOGW("Using software decoded frames (%s) - hardware decoding is unavailable", av_get_pix_fmt_name((AVPixelFormat)fmt));
		}
		int bpp = ten ? 2 : 1;
		for(int p = 0; p < planes; p++)
		{
			int rows = p == 0 ? frame->height : (frame->height + 1) / 2;
			int row_bytes = (p == 0 ? frame->width : (frame->width + 1) / 2) * bpp * ((semi && p == 1) ? 2 : 1);
			D3D11_MAPPED_SUBRESOURCE m;
			if(FAILED(context->Map(sw_planes[p].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
				return result;
			for(int y = 0; y < rows; y++)
				memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, frame->data[p] + (size_t)y * frame->linesize[p], row_bytes);
			context->Unmap(sw_planes[p].Get(), 0);
		}
		YuvMatrix(bt2020, full_range, ten ? 10 : 8, fmt == AV_PIX_FMT_P010LE, matrix);
		ID3D11ShaderResourceView *srvs[3] = { sw_srvs[0].Get(), sw_srvs[1].Get(), sw_srvs[2].Get() };
		ConvertToRgb(srvs, planes, semi, matrix, 1.0f, 1.0f);
	}

	luma_valid[work] = false;
	slot_serial[work] = 0;
	frame_gen_enabled = o.frame_gen;
	if(frame_gen_enabled && EnsureMotionTargets())
	{
		ComputeLumaPyramid(work);
		if(hist[0] >= 0 && luma_valid[hist[0]])
			IssueFrameDiff(work, hist[0]);
	}
	else
		diff_pending[work] = false;

	ProcessFrame(frame, o);

	if(o.ambient)
		UpdateAmbient(o.paper_white);
	else
		amb_valid = false;

	// store: the work slot becomes the newest frame
	result.serial = ++next_serial;
	slot_serial[work] = result.serial;
	hist[2] = hist[1];
	hist[1] = hist[0];
	hist[0] = work;
	latest = work;
	frames_submitted++;
	interp_ready = false;
	return result;
}

// Upscaling / AI processing of the frame that was just converted into rgb[work].
void Renderer::ProcessFrame(AVFrame *frame, const ProcessOptions &o)
{
	float rect[4], uvr[4];
	VideoRect(o.aspect, width, height, rect, uvr);

	// Target size
	int tw = video_w, th = video_h;
	if(o.upscale != UpscaleMode::Off)
	{
		if(o.target_height > 0)
		{
			th = o.target_height;
			tw = (int)std::lround((double)th * video_w / video_h);
		}
		else
		{
			// Match the on-screen video area (accounting for the crop in "fill" mode)
			tw = (int)std::lround(rect[2] / std::max(uvr[2] - uvr[0], 0.01f));
			th = (int)std::lround(rect[3] / std::max(uvr[3] - uvr[1], 0.01f));
		}
		tw = std::min((tw + 1) & ~1, 7680);
		th = std::min((th + 1) & ~1, 4320);
	}
	bool upscaling = o.upscale != UpscaleMode::Off && th > video_h + 8;
	bool hw = frame->format == AV_PIX_FMT_D3D11;
	bool want_ai_sr = upscaling && o.upscale == UpscaleMode::AI && vp->SuperResolutionSupported() && hw;
	bool want_ai_hdr = o.ai_hdr && output_hdr && !video_hdr && vp->AiHdrSupported() && hw;
	if(!upscaling)
	{
		tw = video_w;
		th = video_h;
	}

	if(!upscaling && !want_ai_hdr)
	{
		slot_gen[work] = -1;
		upscaler_status = o.upscale == UpscaleMode::Off ? "Off" : "Not needed (display is not larger than the stream)";
		return;
	}

	int mode_key = (want_ai_sr ? 1 : 0) | (want_ai_hdr ? 2 : 0) | ((int)o.upscale << 2);
	if(tw != last_tw || th != last_th || mode_key != last_mode || want_ai_hdr != last_ai_hdr)
	{
		proc_gen++;
		last_tw = tw;
		last_th = th;
		last_mode = mode_key;
		last_ai_hdr = want_ai_hdr;
	}
	if(proc[work].tex && (proc[work].w != tw || proc[work].h != th))
		vp->Reset(); // its output views reference the texture about to be replaced
	if(!EnsureTarget(proc[work], tw, th, kProcFormat, true, false))
	{
		slot_gen[work] = -1;
		return;
	}

	// 1) Driver AI path: decoder texture -> video processor -> (deband, sharpen) -> proc[work]
	if(want_ai_sr || want_ai_hdr)
	{
		auto *tex = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
		UINT index = (UINT)(intptr_t)frame->data[1];
		bool out_hdr = video_hdr || want_ai_hdr;
		float scale = (float)th / (float)video_h;
		bool post = upscaling && (o.ai_sharpness > 0.01f || o.deband > 0.01f || o.texture_boost > 0.01f || o.clarity > 0.01f);
		// The video processor writes 10-bit RGB where supported (needed for HDR)
		DXGI_FORMAT vfmt = vp->OutputFormatSupported(DXGI_FORMAT_R10G10B10A2_UNORM) ? DXGI_FORMAT_R10G10B10A2_UNORM
			: DXGI_FORMAT_B8G8R8A8_UNORM;
		bool format_ok = vfmt == DXGI_FORMAT_R10G10B10A2_UNORM || !out_hdr;
		bool direct = !post && vfmt == kProcFormat;
		if(!direct && vp_format != vfmt)
		{
			vp->Reset();
			vp_format = vfmt;
		}
		if(!direct && vp_out.tex && (vp_out.w != tw || vp_out.h != th || vp_out.format != vfmt))
			vp->Reset();
		bool targets_ok = direct || EnsureTarget(vp_out, tw, th, vfmt, true, false);
		ID3D11Texture2D *vp_target = direct ? proc[work].tex.Get() : vp_out.tex.Get();
		if(format_ok && targets_ok
			&& vp->Process(tex, index, video_w, video_h, video_hdr, vp_target, tw, th, out_hdr, want_ai_sr, want_ai_hdr))
		{
			if(!direct)
			{
				ID3D11ShaderResourceView *srv = vp_out.srv.Get();
				ProcessOptions po = o;
				if(!upscaling)
					po.texture_boost = po.clarity = 0.0f;
				if(!PostProcess(srv, tw, th, upscaling ? o.deband : 0.0f, scale, upscaling ? o.ai_sharpness : 0.0f, 0.012f, po))
				{
					// nothing to do but format conversion
					DrawFullscreen(ps_sharpen.Get(), proc[work].rtv.Get(), tw, th, &srv, 1, nullptr);
				}
			}
			proc_hdr = out_hdr;
			slot_gen[work] = proc_gen;
			std::string st;
			if(want_ai_sr)
				st = vp->GetVendor() == VpScaler::Vendor::Nvidia ? "NVIDIA RTX Video Super Resolution" : "Intel Video Super Resolution";
			if(want_ai_hdr)
				st += st.empty() ? "NVIDIA RTX Video HDR" : " + RTX Video HDR";
			if(!vp->SuperResolutionActive() && want_ai_sr)
				st += " (driver declined - check driver settings)";
			char res[96];
			snprintf(res, sizeof(res), "  %dx%d  sharpen %.2f  texture %.2f  clarity %.2f", tw, th,
				upscaling ? o.ai_sharpness : 0.0f, upscaling ? o.texture_boost : 0.0f, upscaling ? o.clarity : 0.0f);
			upscaler_status = st + res;
			return;
		}
		// fall through to the shader upscaler
	}

	// 2) Enhanced shader upscaler
	if(upscaling && ShaderUpscale(tw, th, o))
	{
		proc_hdr = video_hdr;
		slot_gen[work] = proc_gen;
		char res[96];
		snprintf(res, sizeof(res), "Enhanced (edge-directed + artifact reduction)  %dx%d", tw, th);
		upscaler_status = res;
		if(o.upscale == UpscaleMode::AI)
			upscaler_status += vp->SuperResolutionSupported() ? "  [AI unavailable for software decoding]"
				: "  [no AI super resolution on this GPU]";
		return;
	}
	slot_gen[work] = -1;
}

bool Renderer::ShaderUpscale(int tw, int th, const ProcessOptions &o)
{
	ID3D11ShaderResourceView *src = rgb[work].srv.Get();

	// Artifact reduction at stream resolution
	if(o.artifact_reduction > 0.01f)
	{
		if(!EnsureTarget(clean, video_w, video_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false))
			return false;
		DenoiseCB d = {};
		d.strength = o.artifact_reduction;
		UpdateCB(cb_denoise.Get(), &d, sizeof(d));
		DrawFullscreen(ps_denoise.Get(), clean.rtv.Get(), video_w, video_h, &src, 1, cb_denoise.Get());
		src = clean.srv.Get();
	}

	// Gradient debanding at stream resolution
	if(o.deband > 0.01f)
	{
		if(!EnsureTarget(clean2, video_w, video_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false))
			return false;
		Deband(src, clean2.rtv.Get(), video_w, video_h, o.deband, 1.0f);
		src = clean2.srv.Get();
	}

	// Edge-directed upscale
	UpCB u = {};
	u.src_size[0] = (float)video_w;
	u.src_size[1] = (float)video_h;
	u.edge_strength = 1.0f;
	UpdateCB(cb_up.Get(), &u, sizeof(u));
	bool sharpen = o.sharpness > 0.01f || o.texture_boost > 0.01f || o.clarity > 0.01f;
	if(sharpen)
	{
		if(!EnsureTarget(up_tmp, tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false))
			return false;
		DrawFullscreen(ps_edge.Get(), up_tmp.rtv.Get(), tw, th, &src, 1, cb_up.Get());

		// Noise-gated detail enhancement at output resolution
		Sharpen(up_tmp.srv.Get(), proc[work].rtv.Get(), tw, th, o.sharpness, 0.008f + 0.02f * o.artifact_reduction, o);
	}
	else
		DrawFullscreen(ps_edge.Get(), proc[work].rtv.Get(), tw, th, &src, 1, cb_up.Get());
	return true;
}

void Renderer::Deband(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, int w, int h, float strength, float scale)
{
	DebandCB d = {};
	d.threshold = 0.004f + 0.014f * strength;  // in signal units (10-bit step = 0.001)
	d.radius = (6.0f + 14.0f * strength) * scale;
	d.seed = (float)(frames_submitted % 997);
	d.grain = 0.0012f;
	UpdateCB(cb_deband.Get(), &d, sizeof(d));
	DrawFullscreen(ps_deband.Get(), dst, w, h, &src, 1, cb_deband.Get());
}

void Renderer::Sharpen(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, int w, int h, float sharpness,
		float noise_floor, const ProcessOptions &o)
{
	// Clarity needs a large-radius blur: build it at 1/16 resolution (two box passes + a soft blur)
	ID3D11ShaderResourceView *clar = nullptr;
	if(o.clarity > 0.01f)
	{
		int w1 = std::max(1, w / 8), h1 = std::max(1, h / 8);
		int w2 = std::max(1, w1 / 2), h2 = std::max(1, h1 / 2);
		if(EnsureTarget(clar1, w1, h1, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true)
			&& EnsureTarget(clar2, w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true)
			&& EnsureTarget(clar3, w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true))
		{
			AmbDownCB dcb = {};
			dcb.decode = 0;
			UpdateCB(cb_amb_down.Get(), &dcb, sizeof(dcb));
			ID3D11ShaderResourceView *s0 = src;
			Dispatch(cs_box8.Get(), &s0, 1, clar1.uav.Get(), cb_amb_down.Get(), w1, h1);
			s0 = clar1.srv.Get();
			Dispatch(cs_box8.Get(), &s0, 1, clar2.uav.Get(), cb_amb_down.Get(), w2, h2);
			AmbCB a = {};
			a.alpha = 1.0f;
			UpdateCB(cb_amb.Get(), &a, sizeof(a));
			ID3D11ShaderResourceView *bs[2] = { clar2.srv.Get(), clar2.srv.Get() };
			Dispatch(cs_amb_blend.Get(), bs, 2, clar3.uav.Get(), cb_amb.Get(), w2, h2);
			clar = clar3.srv.Get();
		}
	}
	SharpCB c = {};
	c.sharpness = std::clamp(sharpness, 0.0f, 2.0f);
	c.noise_floor = noise_floor;
	c.texture_boost = std::clamp(o.texture_boost, 0.0f, 1.0f);
	c.clarity = clar ? std::clamp(o.clarity, 0.0f, 1.0f) : 0.0f;
	c.overshoot = std::clamp(o.overshoot, 0.0f, 1.0f);
	UpdateCB(cb_sharp.Get(), &c, sizeof(c));
	ID3D11ShaderResourceView *srvs[2] = { src, clar };
	DrawFullscreen(ps_sharpen.Get(), dst, w, h, srvs, 2, cb_sharp.Get());
}

bool Renderer::PostProcess(ID3D11ShaderResourceView *src, int tw, int th, float deband, float deband_scale,
		float sharpness, float noise_floor, const ProcessOptions &o)
{
	bool do_deband = deband > 0.01f;
	bool do_sharpen = sharpness > 0.01f || o.texture_boost > 0.01f || o.clarity > 0.01f;
	if(!do_deband && !do_sharpen)
		return false;
	if(do_deband)
	{
		if(do_sharpen)
		{
			if(!EnsureTarget(post_tmp, tw, th, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false))
				return false;
			Deband(src, post_tmp.rtv.Get(), tw, th, deband, deband_scale);
			src = post_tmp.srv.Get();
		}
		else
		{
			Deband(src, proc[work].rtv.Get(), tw, th, deband, deband_scale);
			return true;
		}
	}
	Sharpen(src, proc[work].rtv.Get(), tw, th, sharpness, noise_floor, o);
	return true;
}

void Renderer::UpdateAmbient(float paper_white)
{
	int w1 = std::max(1, video_w / 8), h1 = std::max(1, video_h / 8);
	int w2 = std::max(1, w1 / 4), h2 = std::max(1, h1 / 4);
	if(!EnsureTarget(amb_small1, w1, h1, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true)
		|| !EnsureTarget(amb_small2, w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true)
		|| !EnsureTarget(amb[0], w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true)
		|| !EnsureTarget(amb[1], w2, h2, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true))
		return;
	// level 1 decodes the stream (PQ or gamma) to linear nits, level 2 averages further
	AmbDownCB dcb = {};
	dcb.decode = video_hdr ? 2 : 1;
	dcb.paper_white = paper_white;
	UpdateCB(cb_amb_down.Get(), &dcb, sizeof(dcb));
	ID3D11ShaderResourceView *s = rgb[work].srv.Get();
	Dispatch(cs_box8.Get(), &s, 1, amb_small1.uav.Get(), cb_amb_down.Get(), w1, h1);
	dcb.decode = 0;
	UpdateCB(cb_amb_down.Get(), &dcb, sizeof(dcb));
	s = amb_small1.srv.Get();
	Dispatch(cs_box8.Get(), &s, 1, amb_small2.uav.Get(), cb_amb_down.Get(), w2, h2);

	int next = 1 - amb_index;
	AmbCB a = {};
	a.alpha = amb_valid ? 0.12f : 1.0f; // smooth steady scenes; the shader speeds up on cuts
	UpdateCB(cb_amb.Get(), &a, sizeof(a));
	ID3D11ShaderResourceView *srvs[2] = { amb_small2.srv.Get(), amb[amb_index].srv.Get() };
	Dispatch(cs_amb_blend.Get(), srvs, 2, amb[next].uav.Get(), cb_amb.Get(), w2, h2);
	amb_index = next;
	amb_valid = true;
}

// ---------------------------------------------------------------------------
// Frame generation

bool Renderer::EnsureMotionTargets()
{
	if(mv_coarse[0].tex)
		return true;
	int hw = (video_w + 1) / 2, hh = (video_h + 1) / 2;
	int qw = (hw + 1) / 2, qh = (hh + 1) / 2;
	grid_w = (video_w + kCoarseBlockPx - 1) / kCoarseBlockPx;
	grid_h = (video_h + kCoarseBlockPx - 1) / kCoarseBlockPx;
	fine_w = (video_w + kFineBlockPx - 1) / kFineBlockPx;
	fine_h = (video_h + kFineBlockPx - 1) / kFineBlockPx;
	bool ok = true;
	for(int i = 0; i < kSlots && ok; i++)
	{
		ok = MakeTarget(luma_half[i], hw, hh, DXGI_FORMAT_R16_FLOAT, false, true)
			&& MakeTarget(luma_quarter[i], qw, qh, DXGI_FORMAT_R16_FLOAT, false, true);
		luma_valid[i] = false;
	}
	for(int i = 0; i < 2 && ok; i++)
	{
		ok = MakeTarget(mv_coarse[i], grid_w, grid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true)
			&& MakeTarget(mvc[i].field, fine_w, fine_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true);
		mvc[i].a = mvc[i].b = 0;
	}
	ok = ok && MakeTarget(mv_fine, fine_w, fine_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true);
	if(!ok)
	{
		LOGE("Could not create frame generation textures");
		mv_coarse[0].Reset();
		return false;
	}
	return true;
}

void Renderer::ComputeLumaPyramid(int slot)
{
	ID3D11ShaderResourceView *src = rgb[slot].srv.Get();
	Dispatch(cs_luma_half.Get(), &src, 1, luma_half[slot].uav.Get(), nullptr, luma_half[slot].w, luma_half[slot].h);
	src = luma_half[slot].srv.Get();
	Dispatch(cs_down2.Get(), &src, 1, luma_quarter[slot].uav.Get(), nullptr, luma_quarter[slot].w, luma_quarter[slot].h);
	luma_valid[slot] = true;
}

// Motion between two stored frames, computed once per pair and cached (two pairs are kept: the
// display can be between frames 1-2 or 2-3 of the history).
int Renderer::EnsureMotionField(int sa, int sb)
{
	uint64_t a = slot_serial[sa], b = slot_serial[sb];
	for(int i = 0; i < 2; i++)
		if(mvc[i].a == a && mvc[i].b == b && a)
		{
			mvc[i].used = ++mv_use_counter;
			return i;
		}
	if(!EnsureMotionTargets())
		return -1;
	if(!luma_valid[sa])
		ComputeLumaPyramid(sa);
	if(!luma_valid[sb])
		ComputeLumaPyramid(sb);
	int dst = mvc[0].used <= mvc[1].used ? 0 : 1;
	int other = 1 - dst;
	bool temporal = mvc[other].a != 0;

	// 1) Coarse search at quarter resolution: 4x4 blocks (= 16x16 px), +-8 (= +-32 px half-step,
	//    i.e. up to 64 px of motion per frame), plus the previous pair's vectors for faster motion.
	MeCB me = {};
	me.grid[0] = grid_w;
	me.grid[1] = grid_h;
	me.block = kCoarseBlockPx / 4;
	me.radius = 8;
	me.level_scale = 4;
	me.lambda = 0.2f;
	me.use_pred = 0;
	me.use_temporal = temporal ? 1 : 0;
	me.pred_scale[0] = me.pred_scale[1] = 1.0f;
	me.temporal_scale[0] = (float)fine_w / grid_w;
	me.temporal_scale[1] = (float)fine_h / grid_h;
	UpdateCB(cb_me.Get(), &me, sizeof(me));
	ID3D11ShaderResourceView *srvs[4] = { luma_quarter[sa].srv.Get(), luma_quarter[sb].srv.Get(), nullptr,
		temporal ? mvc[other].field.srv.Get() : nullptr };
	Dispatch(cs_search.Get(), srvs, 4, mv_coarse[0].uav.Get(), cb_me.Get(), grid_w, grid_h);

	ID3D11ShaderResourceView *m = mv_coarse[0].srv.Get();
	Dispatch(cs_median.Get(), &m, 1, mv_coarse[1].uav.Get(), nullptr, grid_w, grid_h);

	// 2) Refine at half resolution on the finer 8x8 px grid, +-2 around the coarse vector.
	me.grid[0] = fine_w;
	me.grid[1] = fine_h;
	me.block = kFineBlockPx / 2;
	me.radius = 2;
	me.level_scale = 2;
	me.lambda = 0.5f;
	me.use_pred = 1;
	me.use_temporal = 0;
	me.pred_scale[0] = (float)grid_w / fine_w;
	me.pred_scale[1] = (float)grid_h / fine_h;
	UpdateCB(cb_me.Get(), &me, sizeof(me));
	ID3D11ShaderResourceView *rsrvs[4] = { luma_half[sa].srv.Get(), luma_half[sb].srv.Get(), mv_coarse[1].srv.Get(), nullptr };
	Dispatch(cs_search.Get(), rsrvs, 4, mv_fine.uav.Get(), cb_me.Get(), fine_w, fine_h);

	m = mv_fine.srv.Get();
	Dispatch(cs_median.Get(), &m, 1, mvc[dst].field.uav.Get(), nullptr, fine_w, fine_h);
	mvc[dst].a = a;
	mvc[dst].b = b;
	mvc[dst].used = ++mv_use_counter;
	return dst;
}

bool Renderer::BuildInterpolated(uint64_t serial_a, uint64_t serial_b, float t)
{
	interp_ready = false;
	if(!frame_gen_enabled)
		return false;
	int sa = FindSlot(serial_a), sb = FindSlot(serial_b);
	if(sa < 0 || sb < 0 || sa == sb)
		return false;
	if(slot_gen[sa] != slot_gen[sb])
		return false; // processing settings changed between the two frames
	int w, h, pw, ph;
	ID3D11ShaderResourceView *b_srv = DisplaySrv(sb, &w, &h);
	ID3D11ShaderResourceView *a_srv = DisplaySrv(sa, &pw, &ph);
	if(w != pw || h != ph)
		return false;
	int field = EnsureMotionField(sa, sb);
	if(field < 0)
		return false;
	DXGI_FORMAT fmt = slot_gen[sb] >= 0 ? kProcFormat : DXGI_FORMAT_R16G16B16A16_FLOAT;
	if(!EnsureTarget(interp, w, h, fmt, true, false))
		return false;

	InterpCB ic = {};
	ic.src_size[0] = (float)video_w;
	ic.src_size[1] = (float)video_h;
	ic.grid_scale[0] = (float)video_w / (float)(fine_w * kFineBlockPx);
	ic.grid_scale[1] = (float)video_h / (float)(fine_h * kFineBlockPx);
	ic.t = std::clamp(t, 0.0f, 2.0f);
	UpdateCB(cb_interp.Get(), &ic, sizeof(ic));
	ID3D11ShaderResourceView *isrvs[3] = { a_srv, b_srv, mvc[field].field.srv.Get() };
	DrawFullscreen(ps_interp.Get(), interp.rtv.Get(), w, h, isrvs, 3, cb_interp.Get());
	interp_ready = true;
	return true;
}

// ---------------------------------------------------------------------------
// Output

void Renderer::BeginOverlay()
{
	const float clear[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(overlay.rtv.Get(), clear);
	ID3D11RenderTargetView *rtv = overlay.rtv.Get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	D3D11_VIEWPORT vpt = { 0, 0, (float)width, (float)height, 0, 1 };
	context->RSSetViewports(1, &vpt);
}

void Renderer::DrawVideoOut(ID3D11RenderTargetView *rtv, int w, int h, ID3D11ShaderResourceView *video, int vw, int vh,
		bool content_hdr, bool out_hdr, const PresentOptions &o, bool with_ambient, bool full_rect)
{
	OutCB c = {};
	float rect[4], uvr[4];
	if(full_rect)
	{
		rect[0] = rect[1] = 0; rect[2] = (float)w; rect[3] = (float)h;
		uvr[0] = uvr[1] = 0; uvr[2] = uvr[3] = 1;
	}
	else
		VideoRect(o.aspect, w, h, rect, uvr);
	memcpy(c.vid_rect, rect, sizeof(rect));
	memcpy(c.uv_rect, uvr, sizeof(uvr));
	c.src_size[0] = (float)vw;
	c.src_size[1] = (float)vh;
	// Bicubic when the picture is enlarged on screen (e.g. a 1440p target on a 4K display)
	float shown_w = rect[2] / std::max(uvr[2] - uvr[0], 0.01f);
	c.scaler = shown_w > vw * 1.01f ? 1 : 0;
	c.mode = content_hdr ? (out_hdr ? 2 : 3) : (out_hdr ? 1 : 0);
	c.paper_white = o.paper_white;
	c.peak = c.mode == 1 ? display_peak_nits : 1000.0f;
	c.ambient = (with_ambient && amb_valid) ? o.ambient : 0.0f;
	c.hdr_boost = o.hdr_boost;
	c.brightness = o.brightness;
	c.contrast = o.contrast;
	c.saturation = o.saturation;
	c.vibrance = o.vibrance;
	c.win_size[0] = (float)w;
	c.win_size[1] = (float)h;
	c.ambient_style = o.ambient_style;
	c.split = (o.compare && frames_submitted > 0) ? rect[0] + rect[2] * std::clamp(o.compare_split, 0.0f, 1.0f) : -1.0f;
	c.orig_mode = video_hdr ? (out_hdr ? 2 : 3) : (out_hdr ? 1 : 0);
	UpdateCB(cb_out.Get(), &c, sizeof(c));
	ID3D11ShaderResourceView *srvs[3] = { video, amb_valid ? amb[amb_index].srv.Get() : nullptr, rgb[display_slot].srv.Get() };
	DrawFullscreen(ps_out.Get(), rtv, w, h, srvs, 3, cb_out.Get());
}

void Renderer::Present(VideoView view, const PresentOptions &opt, uint64_t serial)
{
	if(frame_wait && !frame_waited)
		WaitForSingleObjectEx(frame_wait, 100, TRUE);
	frame_waited = false;

	ID3D11ShaderResourceView *video = nullptr;
	int vw = 0, vh = 0;
	bool content_hdr = false;
	int slot = FindSlot(serial);
	if(slot < 0)
		slot = latest;
	display_slot = slot;
	if(view != VideoView::None && frames_submitted > 0)
	{
		bool processed = slot_gen[slot] >= 0;
		content_hdr = processed ? proc_hdr : video_hdr;
		if(view == VideoView::Interpolated && interp_ready)
		{
			video = interp.srv.Get();
			vw = interp.w;
			vh = interp.h;
		}
		else
			video = DisplaySrv(slot, &vw, &vh);
	}

	const float black[4] = { 0, 0, 0, 1 };
	context->ClearRenderTargetView(bb_rtv.Get(), black);
	if(video)
		DrawVideoOut(bb_rtv.Get(), width, height, video, vw, vh, content_hdr, output_hdr, opt, true, false);

	// UI overlay (premultiplied alpha)
	{
		UiCB u = {};
		u.hdr_out = output_hdr ? 1 : 0;
		u.ui_nits = opt.paper_white;
		UpdateCB(cb_ui.Get(), &u, sizeof(u));
		D3D11_VIEWPORT vpt = { 0, 0, (float)width, (float)height, 0, 1 };
		ID3D11RenderTargetView *rtv = bb_rtv.Get();
		context->RSSetViewports(1, &vpt);
		context->RSSetState(raster.Get());
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->OMSetBlendState(blend_premul.Get(), nullptr, 0xffffffff);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->IASetInputLayout(nullptr);
		context->VSSetShader(vs.Get(), nullptr, 0);
		context->PSSetShader(ps_overlay.Get(), nullptr, 0);
		ID3D11ShaderResourceView *srv = overlay.srv.Get();
		context->PSSetShaderResources(0, 1, &srv);
		ID3D11Buffer *cb = cb_ui.Get();
		context->PSSetConstantBuffers(0, 1, &cb);
		context->Draw(3, 0);
		ID3D11ShaderResourceView *null_srv = nullptr;
		context->PSSetShaderResources(0, 1, &null_srv);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
	}

	UINT flags = 0;
	if(opt.sync_interval == 0 && tearing_supported)
		flags |= DXGI_PRESENT_ALLOW_TEARING;
	HRESULT hr = swapchain->Present(opt.sync_interval, flags);
	if(hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		LOGE("GPU device lost (0x%08lx)", (unsigned long)device->GetDeviceRemovedReason());
}

// ---------------------------------------------------------------------------
// Screenshots

static bool WritePng(const std::wstring &path, int w, int h, const std::vector<uint8_t> &rgba)
{
	bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
	bool ok = false;
	{
		Com<IWICImagingFactory> factory;
		Com<IWICStream> stream;
		Com<IWICBitmapEncoder> encoder;
		Com<IWICBitmapFrameEncode> frame;
		WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
		if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void **)factory.Put()))
			&& SUCCEEDED(factory->CreateStream(stream.Put()))
			&& SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))
			&& SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put()))
			&& SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
			&& SUCCEEDED(encoder->CreateNewFrame(frame.Put(), nullptr))
			&& SUCCEEDED(frame->Initialize(nullptr))
			&& SUCCEEDED(frame->SetSize(w, h))
			&& SUCCEEDED(frame->SetPixelFormat(&fmt))
			&& SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)rgba.size(), (BYTE *)rgba.data()))
			&& SUCCEEDED(frame->Commit())
			&& SUCCEEDED(encoder->Commit()))
			ok = true;
	}
	if(com)
		CoUninitialize();
	return ok;
}

bool Renderer::SaveScreenshot(const std::wstring &path, const PresentOptions &opt)
{
	if(frames_submitted == 0)
		return false;
	int vw, vh;
	ID3D11ShaderResourceView *video = DisplaySrv(latest, &vw, &vh);
	bool content_hdr = slot_gen[latest] >= 0 ? proc_hdr : video_hdr;

	Target shot;
	if(!MakeTarget(shot, vw, vh, DXGI_FORMAT_R8G8B8A8_UNORM, true, false))
		return false;
	PresentOptions o = opt;
	o.aspect = AspectMode::Stretch;
	o.hdr_boost = 0.0f;
	o.compare = false;
	DrawVideoOut(shot.rtv.Get(), vw, vh, video, vw, vh, content_hdr, false, o, false, true);

	D3D11_TEXTURE2D_DESC td;
	shot.tex->GetDesc(&td);
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	Com<ID3D11Texture2D> staging;
	if(FAILED(device->CreateTexture2D(&td, nullptr, staging.Put())))
		return false;
	context->CopyResource(staging.Get(), shot.tex.Get());
	D3D11_MAPPED_SUBRESOURCE m;
	if(FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
		return false;
	std::vector<uint8_t> pixels((size_t)vw * vh * 4);
	for(int y = 0; y < vh; y++)
	{
		uint8_t *row = pixels.data() + (size_t)y * vw * 4;
		memcpy(row, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)vw * 4);
		for(int x = 0; x < vw; x++)
			row[x * 4 + 3] = 255;
	}
	context->Unmap(staging.Get(), 0);

	// Encode off the render thread
	std::thread([path, vw, vh, px = std::move(pixels)]() {
		if(WritePng(path, vw, vh, px))
			LOGI("Screenshot saved (%dx%d)", vw, vh);
		else
			LOGE("Could not save screenshot");
	}).detach();
	return true;
}
