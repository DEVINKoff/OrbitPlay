// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "renderer.h"

#include <map>
#include <utility>

// Driver AI video processing through the D3D11 video processor:
//   - NVIDIA RTX Video Super Resolution (RTX 20-series and newer)
//   - NVIDIA RTX Video HDR (AI SDR -> HDR)
//   - Intel Video Super Resolution (Arc / Iris Xe)
// Same driver hooks Chrome, Edge and VLC use. On other GPUs the processor still scales,
// just without AI, and the renderer uses its own shader upscaler instead.
class VpScaler
{
public:
	enum class Vendor { Unknown, Nvidia, Intel, Amd, Other };

	bool Init(ID3D11Device *device, ID3D11DeviceContext *context);
	Vendor GetVendor() const { return vendor; }
	bool SuperResolutionSupported() const { return vendor == Vendor::Nvidia || vendor == Vendor::Intel; }
	bool AiHdrSupported() const { return vendor == Vendor::Nvidia; }

	// Scales one decoded YUV frame (a slice of the decoder's texture array) into an RGB texture.
	//   in_hdr:  the stream is HDR10 (P010, PQ, BT.2020)
	//   out_hdr: write PQ/BT.2020 output (with ai_hdr, SDR input is converted by RTX Video HDR)
	bool Process(ID3D11Texture2D *in_tex, UINT in_slice, int frame_w, int frame_h, bool in_hdr,
			ID3D11Texture2D *out_tex, int out_w, int out_h, bool out_hdr,
			bool super_resolution, bool ai_hdr);

	// Whether the last Process call had the AI extensions accepted by the driver.
	bool SuperResolutionActive() const { return sr_active; }
	bool AiHdrActive() const { return hdr_active; }

	void Reset();

private:
	bool Configure(UINT in_w, UINT in_h, UINT out_w, UINT out_h);
	void ApplyExtensions(bool super_resolution, bool ai_hdr);

	Com<ID3D11VideoDevice> vdevice;
	Com<ID3D11VideoContext> vcontext;
	Com<ID3D11VideoContext1> vcontext1;
	Com<ID3D11VideoProcessorEnumerator> enumerator;
	Com<ID3D11VideoProcessor> processor;
	UINT cfg_in_w = 0, cfg_in_h = 0, cfg_out_w = 0, cfg_out_h = 0;
	Vendor vendor = Vendor::Unknown;
	bool ext_sr = false, ext_hdr = false, ext_applied = false;
	bool sr_active = false, hdr_active = false;

	std::map<std::pair<ID3D11Texture2D *, UINT>, Com<ID3D11VideoProcessorInputView>> in_views;
	std::map<ID3D11Texture2D *, Com<ID3D11VideoProcessorOutputView>> out_views;
};
