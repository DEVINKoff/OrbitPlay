// SPDX-License-Identifier: AGPL-3.0-only
#include "renderer.h"
#include "log.h"
#include "shaders.h"

#include <d3dcompiler.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Constant buffer layouts (16-byte aligned, must match shaders.h)

struct YuvCB { float rows[12]; float uv_scale[2]; float pad[2]; };
struct CasCB { float sharpness; float pad[3]; };
struct OutCB { float src_size[2]; int scaler; int mode; float paper_white; float content_peak; float pad[2]; };
struct UiCB { int hdr_out; float ui_nits; float pad[2]; };
struct MeCB { int grid[2]; int block; int radius; int level_scale; float lambda; int use_pred; int pad; };
struct InterpCB { float size[2]; float grid_scale[2]; };

static const int kMotionBlockPx = 16; // motion field resolution in full-res pixels

// ---------------------------------------------------------------------------

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
	make_cb(sizeof(CasCB), cb_cas);
	make_cb(sizeof(OutCB), cb_out);
	make_cb(sizeof(UiCB), cb_ui);
	make_cb(sizeof(MeCB), cb_me);
	make_cb(sizeof(InterpCB), cb_interp);

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
		&& ps(shaders::kPsCas, ps_cas)
		&& ps(shaders::kPsVideoOut, ps_out)
		&& ps(shaders::kPsOverlay, ps_overlay)
		&& ps(shaders::kPsInterp, ps_interp)
		&& cs(shaders::kCsLumaHalf, cs_luma_half)
		&& cs(shaders::kCsDown2, cs_down2)
		&& cs(std::string(shaders::kMeCB) + shaders::kCsMotionSearch, cs_search)
		&& cs(shaders::kCsMedian, cs_median);
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
	overlay_rtv.Reset();
	overlay_srv.Reset();
	overlay_tex.Reset();
}

void Renderer::CreateBackbufferTargets()
{
	Com<ID3D11Texture2D> bb;
	swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)bb.Put());
	device->CreateRenderTargetView(bb.Get(), nullptr, bb_rtv.Put());

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	device->CreateTexture2D(&td, nullptr, overlay_tex.Put());
	device->CreateRenderTargetView(overlay_tex.Get(), nullptr, overlay_rtv.Put());
	device->CreateShaderResourceView(overlay_tex.Get(), nullptr, overlay_srv.Put());
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

	// Refresh rate
	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if(GetMonitorInfoW(monitor, &mi))
	{
		DEVMODEW dm = {};
		dm.dmSize = sizeof(dm);
		if(EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
			display_hz = (int)dm.dmDisplayFrequency;
	}

	// HDR state: a fresh factory is needed to see changes to the Windows HDR toggle.
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
	D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
	context->RSSetViewports(1, &vp);
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

static bool MakeTarget(ID3D11Device *dev, int w, int h, DXGI_FORMAT fmt, bool rtv_bind, bool uav_bind,
		Com<ID3D11Texture2D> &tex, Com<ID3D11RenderTargetView> *rtv, Com<ID3D11ShaderResourceView> &srv,
		Com<ID3D11UnorderedAccessView> *uav)
{
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (rtv_bind ? D3D11_BIND_RENDER_TARGET : 0) | (uav_bind ? D3D11_BIND_UNORDERED_ACCESS : 0);
	if(FAILED(dev->CreateTexture2D(&td, nullptr, tex.Put())))
		return false;
	if(rtv_bind && rtv && FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, rtv->Put())))
		return false;
	if(uav_bind && uav && FAILED(dev->CreateUnorderedAccessView(tex.Get(), nullptr, uav->Put())))
		return false;
	return SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, srv.Put()));
}

// YUV -> RGB matrix for normalized texture values.
//   bits: 8 or 10; msb_aligned: 10-bit values stored in the top bits of 16 (P010)
static void YuvMatrix(bool bt2020, bool full_range, int bits, bool msb_aligned, float out[12])
{
	double kr = bt2020 ? 0.2627 : 0.2126;
	double kb = bt2020 ? 0.0593 : 0.0722;
	double kg = 1.0 - kr - kb;

	double scale; // normalized texture value -> integer code
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

	double ya = scale / yrange, yb = -yoff / yrange;  // y' = ya*n + yb
	double ca = scale / crange, cb = -coff / crange;  // c  = ca*n + cb

	double r_cr = 2.0 * (1.0 - kr);
	double b_cb = 2.0 * (1.0 - kb);
	double g_cb = -2.0 * kb * (1.0 - kb) / kg;
	double g_cr = -2.0 * kr * (1.0 - kr) / kg;

	// rows: (n_y, n_u, n_v, 1)
	out[0] = (float)ya; out[1] = 0.0f;               out[2] = (float)(r_cr * ca);  out[3] = (float)(yb + r_cr * cb);
	out[4] = (float)ya; out[5] = (float)(g_cb * ca); out[6] = (float)(g_cr * ca);  out[7] = (float)(yb + (g_cb + g_cr) * cb);
	out[8] = (float)ya; out[9] = (float)(b_cb * ca); out[10] = 0.0f;               out[11] = (float)(yb + b_cb * cb);
}

// ---------------------------------------------------------------------------
// Video

void Renderer::ResetVideo()
{
	frames_submitted = 0;
	interp_ready = false;
	luma_valid[0] = luma_valid[1] = false;
}

bool Renderer::EnsureVideoTargets(int w, int h)
{
	if(w == video_w && h == video_h && rgb_tex[0])
		return true;
	video_w = w;
	video_h = h;
	frames_submitted = 0;
	interp_ready = false;
	for(int i = 0; i < 2; i++)
	{
		if(!MakeTarget(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, rgb_tex[i], &rgb_rtv[i], rgb_srv[i], nullptr))
			return false;
		luma_valid[i] = false;
	}
	if(!MakeTarget(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, cas_tex, &cas_rtv, cas_srv, nullptr))
		return false;
	// Interpolation targets depend on the stream size too.
	interp_tex.Reset();
	LOGI("Video size %dx%d", w, h);
	return true;
}

bool Renderer::EnsureInterpTargets()
{
	if(interp_tex)
		return true;
	int hw = (video_w + 1) / 2, hh = (video_h + 1) / 2;
	int qw = (hw + 1) / 2, qh = (hh + 1) / 2;
	grid_w = (video_w + kMotionBlockPx - 1) / kMotionBlockPx;
	grid_h = (video_h + kMotionBlockPx - 1) / kMotionBlockPx;
	for(int i = 0; i < 2; i++)
	{
		if(!MakeTarget(device.Get(), hw, hh, DXGI_FORMAT_R16_FLOAT, false, true, luma_half_tex[i], nullptr, luma_half_srv[i], &luma_half_uav[i])
			|| !MakeTarget(device.Get(), qw, qh, DXGI_FORMAT_R16_FLOAT, false, true, luma_quarter_tex[i], nullptr, luma_quarter_srv[i], &luma_quarter_uav[i])
			|| !MakeTarget(device.Get(), grid_w, grid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, true, mv_tex[i], nullptr, mv_srv[i], &mv_uav[i]))
		{
			LOGE("Could not create frame generation textures");
			return false;
		}
		luma_valid[i] = false;
	}
	if(!MakeTarget(device.Get(), video_w, video_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, false, interp_tex, &interp_rtv, interp_srv, nullptr))
		return false;
	return true;
}

void Renderer::ConvertToRgb(ID3D11ShaderResourceView *const *planes, int plane_count, bool semi_planar,
		const float matrix[12], float uv_sx, float uv_sy)
{
	YuvCB cb = {};
	memcpy(cb.rows, matrix, sizeof(cb.rows));
	cb.uv_scale[0] = uv_sx;
	cb.uv_scale[1] = uv_sy;
	UpdateCB(cb_yuv.Get(), &cb, sizeof(cb));

	latest = (int)(frames_submitted % 2);
	DrawFullscreen(semi_planar ? ps_nv12.Get() : ps_planar.Get(), rgb_rtv[latest].Get(), video_w, video_h,
			planes, plane_count, cb_yuv.Get());
}

bool Renderer::SubmitFrame(AVFrame *frame)
{
	if(!frame || frame->width <= 0 || frame->height <= 0)
		return false;
	if(!EnsureVideoTargets(frame->width, frame->height))
		return false;

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
		// Hardware frame: data[0] = texture array, data[1] = slice index
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
				return false;
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
		// Software frame fallback
		int fmt = frame->format;
		bool semi = fmt == AV_PIX_FMT_NV12 || fmt == AV_PIX_FMT_P010LE;
		bool ten = fmt == AV_PIX_FMT_YUV420P10LE || fmt == AV_PIX_FMT_P010LE;
		bool planar = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUV420P10LE;
		if(!semi && !planar)
		{
			LOGE("Unsupported decoder output format %s", av_get_pix_fmt_name((AVPixelFormat)fmt));
			return false;
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
					return false;
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
				return false;
			for(int y = 0; y < rows; y++)
				memcpy((uint8_t *)m.pData + (size_t)y * m.RowPitch, frame->data[p] + (size_t)y * frame->linesize[p], row_bytes);
			context->Unmap(sw_planes[p].Get(), 0);
		}
		YuvMatrix(bt2020, full_range, ten ? 10 : 8, fmt == AV_PIX_FMT_P010LE, matrix);
		ID3D11ShaderResourceView *srvs[3] = { sw_srvs[0].Get(), sw_srvs[1].Get(), sw_srvs[2].Get() };
		ConvertToRgb(srvs, planes, semi, matrix, 1.0f, 1.0f);
	}

	frames_submitted++;
	interp_ready = false;

	if(frame_gen_enabled && EnsureInterpTargets())
		ComputeLumaPyramid(latest);
	return true;
}

void Renderer::ComputeLumaPyramid(int slot)
{
	D3D11_TEXTURE2D_DESC hd, qd;
	luma_half_tex[slot]->GetDesc(&hd);
	luma_quarter_tex[slot]->GetDesc(&qd);
	ID3D11ShaderResourceView *src = rgb_srv[slot].Get();
	Dispatch(cs_luma_half.Get(), &src, 1, luma_half_uav[slot].Get(), nullptr, hd.Width, hd.Height);
	src = luma_half_srv[slot].Get();
	Dispatch(cs_down2.Get(), &src, 1, luma_quarter_uav[slot].Get(), nullptr, qd.Width, qd.Height);
	luma_valid[slot] = true;
}

bool Renderer::BuildInterpolatedFrame()
{
	if(interp_ready)
		return true;
	if(!frame_gen_enabled || frames_submitted < 2 || !EnsureInterpTargets())
		return false;
	int cur = latest, prev = 1 - latest;
	if(!luma_valid[cur])
		ComputeLumaPyramid(cur);
	if(!luma_valid[prev])
		ComputeLumaPyramid(prev);

	// 1) Coarse search at quarter resolution: 4x4 px blocks (= 16x16 full-res), +-8 px (= +-32 full-res).
	MeCB me = {};
	me.grid[0] = grid_w;
	me.grid[1] = grid_h;
	me.block = kMotionBlockPx / 4;
	me.radius = 8;
	me.level_scale = 4;
	me.lambda = 0.2f;
	me.use_pred = 0;
	UpdateCB(cb_me.Get(), &me, sizeof(me));
	ID3D11ShaderResourceView *srvs[3] = { luma_quarter_srv[prev].Get(), luma_quarter_srv[cur].Get(), nullptr };
	Dispatch(cs_search.Get(), srvs, 3, mv_uav[0].Get(), cb_me.Get(), grid_w, grid_h);

	// 2) Remove outliers
	ID3D11ShaderResourceView *msrc = mv_srv[0].Get();
	Dispatch(cs_median.Get(), &msrc, 1, mv_uav[1].Get(), nullptr, grid_w, grid_h);

	// 3) Refine at half resolution around the coarse vector: 8x8 px blocks, +-2 px.
	me.block = kMotionBlockPx / 2;
	me.radius = 2;
	me.level_scale = 2;
	me.lambda = 0.5f;
	me.use_pred = 1;
	UpdateCB(cb_me.Get(), &me, sizeof(me));
	ID3D11ShaderResourceView *rsrvs[3] = { luma_half_srv[prev].Get(), luma_half_srv[cur].Get(), mv_srv[1].Get() };
	Dispatch(cs_search.Get(), rsrvs, 3, mv_uav[0].Get(), cb_me.Get(), grid_w, grid_h);

	msrc = mv_srv[0].Get();
	Dispatch(cs_median.Get(), &msrc, 1, mv_uav[1].Get(), nullptr, grid_w, grid_h);

	// 4) Warp both frames to the midpoint and blend
	InterpCB ic = {};
	ic.size[0] = (float)video_w;
	ic.size[1] = (float)video_h;
	ic.grid_scale[0] = (float)video_w / (float)(grid_w * kMotionBlockPx);
	ic.grid_scale[1] = (float)video_h / (float)(grid_h * kMotionBlockPx);
	UpdateCB(cb_interp.Get(), &ic, sizeof(ic));
	ID3D11ShaderResourceView *isrvs[3] = { rgb_srv[prev].Get(), rgb_srv[cur].Get(), mv_srv[1].Get() };
	DrawFullscreen(ps_interp.Get(), interp_rtv.Get(), video_w, video_h, isrvs, 3, cb_interp.Get());

	interp_ready = true;
	return true;
}

// ---------------------------------------------------------------------------
// Output

void Renderer::BeginOverlay()
{
	const float clear[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(overlay_rtv.Get(), clear);
	ID3D11RenderTargetView *rtv = overlay_rtv.Get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	D3D11_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0, 1 };
	context->RSSetViewports(1, &vp);
}

void Renderer::Present(VideoView view, const RenderOptions &opt)
{
	if(frame_wait)
		WaitForSingleObjectEx(frame_wait, 100, TRUE);

	ID3D11ShaderResourceView *video = nullptr;
	if(view == VideoView::Interpolated && interp_ready)
		video = interp_srv.Get();
	else if(view != VideoView::None && frames_submitted > 0)
		video = rgb_srv[latest].Get();

	// Optional sharpening at stream resolution before upscaling
	if(video && opt.upscale && opt.sharpness > 0.01f)
	{
		CasCB c = {};
		c.sharpness = opt.sharpness;
		UpdateCB(cb_cas.Get(), &c, sizeof(c));
		DrawFullscreen(ps_cas.Get(), cas_rtv.Get(), video_w, video_h, &video, 1, cb_cas.Get());
		video = cas_srv.Get();
	}

	const float black[4] = { 0, 0, 0, 1 };
	context->ClearRenderTargetView(bb_rtv.Get(), black);

	if(video)
	{
		// Letterbox to the stream aspect ratio
		float aspect = (float)video_w / (float)video_h;
		float vw = (float)width, vh = vw / aspect;
		if(vh > height)
		{
			vh = (float)height;
			vw = vh * aspect;
		}
		float vx = std::floor((width - vw) * 0.5f), vy = std::floor((height - vh) * 0.5f);

		OutCB o = {};
		o.src_size[0] = (float)video_w;
		o.src_size[1] = (float)video_h;
		o.scaler = opt.upscale ? 1 : 0;
		o.mode = video_hdr ? (output_hdr ? 2 : 3) : (output_hdr ? 1 : 0);
		o.paper_white = opt.paper_white;
		o.content_peak = 1000.0f;
		UpdateCB(cb_out.Get(), &o, sizeof(o));

		D3D11_VIEWPORT vp = { vx, vy, vw, vh, 0, 1 };
		ID3D11RenderTargetView *rtv = bb_rtv.Get();
		context->RSSetViewports(1, &vp);
		context->RSSetState(raster.Get());
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->IASetInputLayout(nullptr);
		context->VSSetShader(vs.Get(), nullptr, 0);
		context->PSSetShader(ps_out.Get(), nullptr, 0);
		ID3D11SamplerState *s = samp_linear.Get();
		context->PSSetSamplers(0, 1, &s);
		context->PSSetShaderResources(0, 1, &video);
		ID3D11Buffer *cb = cb_out.Get();
		context->PSSetConstantBuffers(0, 1, &cb);
		context->Draw(3, 0);
		ID3D11ShaderResourceView *null_srv = nullptr;
		context->PSSetShaderResources(0, 1, &null_srv);
	}

	// UI overlay (premultiplied alpha)
	{
		UiCB u = {};
		u.hdr_out = output_hdr ? 1 : 0;
		u.ui_nits = opt.paper_white;
		UpdateCB(cb_ui.Get(), &u, sizeof(u));
		D3D11_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0, 1 };
		ID3D11RenderTargetView *rtv = bb_rtv.Get();
		context->RSSetViewports(1, &vp);
		context->OMSetRenderTargets(1, &rtv, nullptr);
		context->OMSetBlendState(blend_premul.Get(), nullptr, 0xffffffff);
		context->VSSetShader(vs.Get(), nullptr, 0);
		context->PSSetShader(ps_overlay.Get(), nullptr, 0);
		ID3D11ShaderResourceView *srv = overlay_srv.Get();
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
