// SPDX-License-Identifier: AGPL-3.0-only
#include "vpscaler.h"
#include "log.h"

// Vendor extension interfaces, as used by Chromium (ui/gl/swap_chain_presenter.cc).
static const GUID kNvidiaPPEInterfaceGUID = { 0xd43ce1b3, 0x1f4b, 0x48ac, { 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7 } };
static const GUID kNvidiaTrueHDRInterfaceGUID = { 0xfdd62bb4, 0x620b, 0x4fd7, { 0x9a, 0xb3, 0x1e, 0x59, 0xd0, 0xd5, 0x44, 0xb3 } };
static const GUID kIntelVpeInterfaceGUID = { 0xedd1d4b9, 0x8659, 0x4cbc, { 0xa4, 0xd6, 0x98, 0x31, 0xa2, 0x16, 0x3a, 0xc3 } };

enum : UINT { kIntelVpeFnVersion = 0x01, kIntelVpeFnMode = 0x20, kIntelVpeFnScaling = 0x37 };
enum : UINT { kIntelVpeVersion3 = 0x0003 };
enum : UINT { kIntelVpeModeNone = 0x0, kIntelVpeModePreproc = 0x01 };
enum : UINT { kIntelVpeScalingDefault = 0x0, kIntelVpeScalingSuperResolution = 0x2 };
struct IntelVpeExt { UINT function; void *param; };

bool VpScaler::Init(ID3D11Device *device, ID3D11DeviceContext *context)
{
	if(FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), (void **)vdevice.Put()))
		|| FAILED(context->QueryInterface(__uuidof(ID3D11VideoContext), (void **)vcontext.Put())))
	{
		LOGW("D3D11 video processing is not available");
		return false;
	}
	context->QueryInterface(__uuidof(ID3D11VideoContext1), (void **)vcontext1.Put());

	Com<IDXGIDevice> dxgi;
	Com<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc = {};
	if(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)dxgi.Put()))
		&& SUCCEEDED(dxgi->GetAdapter(adapter.Put())) && SUCCEEDED(adapter->GetDesc(&desc)))
	{
		switch(desc.VendorId)
		{
			case 0x10DE: vendor = Vendor::Nvidia; break;
			case 0x8086: vendor = Vendor::Intel; break;
			case 0x1002: case 0x1022: vendor = Vendor::Amd; break;
			default: vendor = Vendor::Other; break;
		}
		char name[128] = {};
		WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
		LOGI("GPU: %s", name);
	}
	return true;
}

void VpScaler::Reset()
{
	in_views.clear();
	out_views.clear();
	processor.Reset();
	enumerator.Reset();
	cfg_in_w = cfg_in_h = cfg_out_w = cfg_out_h = 0;
	ext_applied = false;
}

bool VpScaler::Configure(UINT in_w, UINT in_h, UINT out_w, UINT out_h)
{
	if(processor && in_w == cfg_in_w && in_h == cfg_in_h && out_w == cfg_out_w && out_h == cfg_out_h)
		return true;
	Reset();
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
	cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	cd.InputFrameRate = { 60, 1 };
	cd.InputWidth = in_w;
	cd.InputHeight = in_h;
	cd.OutputFrameRate = { 60, 1 };
	cd.OutputWidth = out_w;
	cd.OutputHeight = out_h;
	cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if(FAILED(vdevice->CreateVideoProcessorEnumerator(&cd, enumerator.Put()))
		|| FAILED(vdevice->CreateVideoProcessor(enumerator.Get(), 0, processor.Put())))
	{
		LOGW("Could not create a video processor for %ux%u -> %ux%u", in_w, in_h, out_w, out_h);
		Reset();
		return false;
	}
	cfg_in_w = in_w;
	cfg_in_h = in_h;
	cfg_out_w = out_w;
	cfg_out_h = out_h;

	// We handle everything else ourselves; keep the driver from "enhancing" colors on its own.
	vcontext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
	vcontext->VideoProcessorSetStreamFrameFormat(processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	vcontext->VideoProcessorSetStreamOutputRate(processor.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
	return true;
}

void VpScaler::ApplyExtensions(bool super_resolution, bool ai_hdr)
{
	if(ext_applied && super_resolution == ext_sr && ai_hdr == ext_hdr)
		return;
	ext_sr = super_resolution;
	ext_hdr = ai_hdr;
	ext_applied = true;
	sr_active = hdr_active = false;

	if(vendor == Vendor::Nvidia)
	{
		struct { UINT version; UINT method; UINT enable; } sr = { 0x1, 0x2, super_resolution ? 1u : 0u };
		HRESULT hr = vcontext->VideoProcessorSetStreamExtension(processor.Get(), 0, &kNvidiaPPEInterfaceGUID, sizeof(sr), &sr);
		sr_active = super_resolution && SUCCEEDED(hr);
		if(super_resolution)
			LOGI("NVIDIA RTX Video Super Resolution: %s (0x%08lx)", SUCCEEDED(hr) ? "requested" : "rejected", (unsigned long)hr);

		struct { UINT version; UINT method; UINT enable : 1; UINT reserved : 31; } hdr = { 0x4, 0x3, ai_hdr ? 1u : 0u, 0 };
		hr = vcontext->VideoProcessorSetStreamExtension(processor.Get(), 0, &kNvidiaTrueHDRInterfaceGUID, sizeof(hdr), &hdr);
		hdr_active = ai_hdr && SUCCEEDED(hr);
		if(ai_hdr)
			LOGI("NVIDIA RTX Video HDR: %s (0x%08lx)", SUCCEEDED(hr) ? "requested" : "rejected", (unsigned long)hr);
	}
	else if(vendor == Vendor::Intel)
	{
		UINT param = 0;
		IntelVpeExt ext = { 0, &param };
		ext.function = kIntelVpeFnVersion;
		param = kIntelVpeVersion3;
		HRESULT hr = vcontext->VideoProcessorSetOutputExtension(processor.Get(), &kIntelVpeInterfaceGUID, sizeof(ext), &ext);
		if(SUCCEEDED(hr))
		{
			ext.function = kIntelVpeFnMode;
			param = super_resolution ? kIntelVpeModePreproc : kIntelVpeModeNone;
			hr = vcontext->VideoProcessorSetOutputExtension(processor.Get(), &kIntelVpeInterfaceGUID, sizeof(ext), &ext);
		}
		if(SUCCEEDED(hr))
		{
			ext.function = kIntelVpeFnScaling;
			param = super_resolution ? kIntelVpeScalingSuperResolution : kIntelVpeScalingDefault;
			hr = vcontext->VideoProcessorSetStreamExtension(processor.Get(), 0, &kIntelVpeInterfaceGUID, sizeof(ext), &ext);
		}
		sr_active = super_resolution && SUCCEEDED(hr);
		if(super_resolution)
			LOGI("Intel Video Super Resolution: %s (0x%08lx)", SUCCEEDED(hr) ? "requested" : "rejected", (unsigned long)hr);
	}
}

bool VpScaler::Process(ID3D11Texture2D *in_tex, UINT in_slice, int frame_w, int frame_h, bool in_hdr,
		ID3D11Texture2D *out_tex, int out_w, int out_h, bool out_hdr, bool super_resolution, bool ai_hdr)
{
	if(!vdevice)
		return false;
	if(!Configure((UINT)frame_w, (UINT)frame_h, (UINT)out_w, (UINT)out_h))
		return false;

	auto key = std::make_pair(in_tex, in_slice);
	auto it = in_views.find(key);
	if(it == in_views.end())
	{
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
		ivd.FourCC = 0;
		ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		ivd.Texture2D.MipSlice = 0;
		ivd.Texture2D.ArraySlice = in_slice;
		Com<ID3D11VideoProcessorInputView> view;
		if(FAILED(vdevice->CreateVideoProcessorInputView(in_tex, enumerator.Get(), &ivd, view.Put())))
		{
			LOGW("Video processor cannot read the decoder texture");
			return false;
		}
		it = in_views.emplace(key, view).first;
	}

	auto ot = out_views.find(out_tex);
	if(ot == out_views.end())
	{
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
		ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		Com<ID3D11VideoProcessorOutputView> view;
		if(FAILED(vdevice->CreateVideoProcessorOutputView(out_tex, enumerator.Get(), &ovd, view.Put())))
		{
			LOGW("Video processor cannot write the output texture");
			return false;
		}
		ot = out_views.emplace(out_tex, view).first;
	}

	RECT src = { 0, 0, frame_w, frame_h };
	RECT dst = { 0, 0, out_w, out_h };
	vcontext->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src);
	vcontext->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst);
	vcontext->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &dst);

	if(vcontext1)
	{
		vcontext1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0,
			in_hdr ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
		vcontext1->VideoProcessorSetOutputColorSpace1(processor.Get(),
			out_hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	}
	else
	{
		D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs = {};
		in_cs.YCbCr_Matrix = 1; // BT.709
		in_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
		vcontext->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &in_cs);
		D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs = {};
		out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
		vcontext->VideoProcessorSetOutputColorSpace(processor.Get(), &out_cs);
	}

	ApplyExtensions(super_resolution, ai_hdr && !in_hdr && out_hdr);

	D3D11_VIDEO_PROCESSOR_STREAM stream = {};
	stream.Enable = TRUE;
	stream.pInputSurface = it->second.Get();
	HRESULT hr = vcontext->VideoProcessorBlt(processor.Get(), ot->second.Get(), 0, 1, &stream);
	if(FAILED(hr))
	{
		LOGW("VideoProcessorBlt failed (0x%08lx)", (unsigned long)hr);
		return false;
	}
	return true;
}
