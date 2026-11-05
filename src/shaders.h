// SPDX-License-Identifier: AGPL-3.0-only
// HLSL sources, compiled at startup with D3DCompile (d3dcompiler_47.dll ships with Windows 10/11).
#pragma once

namespace shaders {

// Shared helpers -------------------------------------------------------------
static const char *kCommon = R"HLSL(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

static const float PQ_M1 = 0.1593017578125;
static const float PQ_M2 = 78.84375;
static const float PQ_C1 = 0.8359375;
static const float PQ_C2 = 18.8515625;
static const float PQ_C3 = 18.6875;

// PQ signal -> linear light, 1.0 = 10000 nits
float3 PqToLinear(float3 n)
{
	float3 p = pow(saturate(n), 1.0 / PQ_M2);
	return pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
}

// linear light (1.0 = 10000 nits) -> PQ signal
float3 LinearToPq(float3 l)
{
	float3 lm = pow(saturate(l), PQ_M1);
	return pow((PQ_C1 + PQ_C2 * lm) / (1.0 + PQ_C3 * lm), PQ_M2);
}

float3 SrgbToLinear(float3 c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static const float3x3 BT709_TO_BT2020 = float3x3(
	0.6274040, 0.3292820, 0.0433136,
	0.0690970, 0.9195400, 0.0113612,
	0.0163916, 0.0880132, 0.8955950);

static const float3x3 BT2020_TO_BT709 = float3x3(
	 1.6604910, -0.5876411, -0.0728499,
	-0.1245505,  1.1328999, -0.0083494,
	-0.0181508, -0.1005789,  1.1187297);

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
)HLSL";

// Fullscreen triangle --------------------------------------------------------
static const char *kVS = R"HLSL(
VSOut main(uint id : SV_VertexID)
{
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.uv = uv;
	o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}
)HLSL";

// YUV -> RGB (output stays in the stream's transfer: gamma for SDR, PQ for HDR) ------
static const char *kYuvCB = R"HLSL(
cbuffer YuvCB : register(b0)
{
	float4 row0;
	float4 row1;
	float4 row2;
	float2 uv_scale;
	float2 pad0;
};
SamplerState samp : register(s0);
)HLSL";

// Semi-planar (NV12 / P010) from the hardware decoder
static const char *kPsYuvNv12 = R"HLSL(
Texture2D<float> texY : register(t0);
Texture2D<float2> texUV : register(t1);
float4 main(VSOut i) : SV_Target
{
	float2 uv = i.uv * uv_scale;
	float4 yuv = float4(texY.SampleLevel(samp, uv, 0), texUV.SampleLevel(samp, uv, 0), 1.0);
	return float4(saturate(float3(dot(row0, yuv), dot(row1, yuv), dot(row2, yuv))), 1.0);
}
)HLSL";

// Planar (YUV420P / YUV420P10) from the software decoder fallback
static const char *kPsYuvPlanar = R"HLSL(
Texture2D<float> texY : register(t0);
Texture2D<float> texU : register(t1);
Texture2D<float> texV : register(t2);
float4 main(VSOut i) : SV_Target
{
	float2 uv = i.uv * uv_scale;
	float4 yuv = float4(texY.SampleLevel(samp, uv, 0), texU.SampleLevel(samp, uv, 0), texV.SampleLevel(samp, uv, 0), 1.0);
	return float4(saturate(float3(dot(row0, yuv), dot(row1, yuv), dot(row2, yuv))), 1.0);
}
)HLSL";

static const char *kPsOverlay = R"HLSL(
Texture2D<float4> ui : register(t0);
cbuffer UiCB : register(b0) { int hdr_out; float ui_nits; float2 pad3; };
float4 main(VSOut i) : SV_Target
{
	float4 c = ui.Load(int3(int2(i.pos.xy), 0));
	if(hdr_out != 0 && c.a > 0.0)
	{
		float3 straight = saturate(c.rgb / c.a);
		float3 lin = mul(BT709_TO_BT2020, SrgbToLinear(straight)) * (ui_nits / 10000.0);
		c.rgb = LinearToPq(lin) * c.a;
	}
	return c;
}
)HLSL";

// ---------------------------------------------------------------------------
// Enhanced upscaler (used when no AI super resolution is available):
//   1) artifact reduction at stream resolution (edge-preserving, removes blocking/mosquito noise)
//   2) edge-directed upscale (anisotropic Lanczos kernel stretched along edges)
//   3) noise-gated luma sharpening at output resolution (no halos, doesn't amplify grain)

static const char *kPsDenoise = R"HLSL(
Texture2D<float4> src : register(t0);
cbuffer DenoiseCB : register(b0) { float strength; float3 padd; };
float4 main(VSOut i) : SV_Target
{
	int2 p = int2(i.pos.xy);
	int w, h; src.GetDimensions(w, h);
	int2 m = int2(w - 1, h - 1);
	float3 c = src.Load(int3(p, 0)).rgb;
	float lc = Luma(c);
	float sigma = lerp(0.012, 0.07, saturate(strength));
	float inv2s2 = 1.0 / (2.0 * sigma * sigma);
	float3 acc = c;
	float wsum = 1.0;
	[unroll] for(int y = -1; y <= 1; y++)
		[unroll] for(int x = -1; x <= 1; x++)
		{
			if(x == 0 && y == 0) continue;
			float3 n = src.Load(int3(clamp(p + int2(x, y), 0, m), 0)).rgb;
			float dl = Luma(n) - lc;
			float spatial = (x == 0 || y == 0) ? 0.6 : 0.35;
			float wgt = spatial * exp(-dl * dl * inv2s2);
			acc += n * wgt;
			wsum += wgt;
		}
	return float4(acc / wsum, 1.0);
}
)HLSL";

static const char *kPsEdgeUpscale = R"HLSL(
Texture2D<float4> src : register(t0);
cbuffer UpCB : register(b0) { float2 src_size; float edge_strength; float padu; };

float SincU(float x) { x *= 3.14159265; return abs(x) < 1e-4 ? 1.0 : sin(x) / x; }
float Lanczos2R(float r) { return r < 2.0 ? SincU(r) * SincU(r * 0.5) : 0.0; }

// Edge-directed upscale. Edge orientation comes from the local structure tensor (gradients
// summed over the 2x2 texels around the sample, bilinearly weighted). Its coherence says how
// "line-like" the area is: the kernel gets narrower across lines and longer along them, so
// diagonal edges come out smooth and crisp while textures and flat areas get a plain Lanczos.
float4 main(VSOut i) : SV_Target
{
	float2 pos = i.uv * src_size - 0.5;
	float2 base = floor(pos);
	float2 f = pos - base;
	int2 maxc = int2(src_size) - 1;

	float3 c[4][4];
	float L[4][4];
	[unroll] for(int y = 0; y < 4; y++)
		[unroll] for(int x = 0; x < 4; x++)
		{
			c[y][x] = src.Load(int3(clamp(int2(base) + int2(x - 1, y - 1), 0, maxc), 0)).rgb;
			L[y][x] = Luma(c[y][x]);
		}

	float wts[2][2] = { { (1 - f.x) * (1 - f.y), f.x * (1 - f.y) }, { (1 - f.x) * f.y, f.x * f.y } };
	float jxx = 0, jxy = 0, jyy = 0;
	[unroll] for(int yy = 1; yy <= 2; yy++)
		[unroll] for(int xx = 1; xx <= 2; xx++)
		{
			float gx = (L[yy][xx + 1] - L[yy][xx - 1]) * 0.5;
			float gy = (L[yy + 1][xx] - L[yy - 1][xx]) * 0.5;
			float w = wts[yy - 1][xx - 1];
			jxx += gx * gx * w; jxy += gx * gy * w; jyy += gy * gy * w;
		}
	// eigen decomposition of [[jxx, jxy], [jxy, jyy]]
	float tr = jxx + jyy;
	float dt = sqrt(max((jxx - jyy) * (jxx - jyy) + 4.0 * jxy * jxy, 0.0));
	float l1 = 0.5 * (tr + dt), l2 = 0.5 * (tr - dt);
	float coherence = tr > 1e-7 ? (l1 - l2) / (l1 + l2 + 1e-7) : 0.0;
	float2 n = abs(jxy) > 1e-7 ? normalize(float2(l1 - jyy, jxy)) : (jxx >= jyy ? float2(1, 0) : float2(0, 1));
	float2 t = float2(-n.y, n.x);
	// only real edges (enough contrast) get the directional kernel
	float s = coherence * coherence * saturate(sqrt(l1) * 24.0) * edge_strength;
	float across = 1.0 + 0.6 * s;
	float along = 1.0 - 0.55 * s;

	float3 acc = 0.0;
	float wsum = 0.0;
	[unroll] for(int y2 = 0; y2 < 4; y2++)
		[unroll] for(int x2 = 0; x2 < 4; x2++)
		{
			float2 d = float2(x2 - 1, y2 - 1) - f;
			float a = dot(d, n) * across;
			float b = dot(d, t) * along;
			float wgt = Lanczos2R(sqrt(a * a + b * b));
			acc += c[y2][x2] * wgt;
			wsum += wgt;
		}
	float3 r = acc / max(wsum, 1e-4);
	float3 mn = min(min(c[1][1], c[1][2]), min(c[2][1], c[2][2]));
	float3 mx = max(max(c[1][1], c[1][2]), max(c[2][1], c[2][2]));
	return float4(clamp(r, mn, mx), 1.0);
}
)HLSL";

// Robust contrast-adaptive sharpening (algorithm after AMD FidelityFX RCAS, MIT license).
// Sharpens only as far as it can without clipping, and backs off on noise, so it adds
// crispness to the AI/upscaled image without halos or grain.
static const char *kPsSharpen = R"HLSL(
Texture2D<float4> src : register(t0);
Texture2D<float4> clarityBlur : register(t1);   // large-radius blur of the image (low res)
SamplerState samp : register(s0);
cbuffer SharpCB : register(b0)
{
	float sharpness;      // 0..2  edges (RCAS + 5x5 detail)
	float noise_floor;
	float texture_boost;  // 0..1  fine texture (1 px scale)
	float clarity;        // 0..1  local contrast (large radius)
	float overshoot;      // 0..1  how far past the local min/max sharpening may go (0 = no halos)
	float3 pads;
};
float Max3(float3 v) { return max(v.x, max(v.y, v.z)); }
float4 main(VSOut i) : SV_Target
{
	int2 p = int2(i.pos.xy);
	int w, h; src.GetDimensions(w, h);
	int2 m = int2(w - 1, h - 1);
	float3 b = src.Load(int3(clamp(p + int2(0, -1), 0, m), 0)).rgb;
	float3 d = src.Load(int3(clamp(p + int2(-1, 0), 0, m), 0)).rgb;
	float3 e = src.Load(int3(p, 0)).rgb;
	float3 f = src.Load(int3(clamp(p + int2(1, 0), 0, m), 0)).rgb;
	float3 hh = src.Load(int3(clamp(p + int2(0, 1), 0, m), 0)).rgb;

	// 1) RCAS: largest negative lobe that keeps the result inside [0, 1]
	float3 mn4 = min(min(b, d), min(f, hh));
	float3 mx4 = max(max(b, d), max(f, hh));
	float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
	float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
	float3 lobeRGB = max(-hitMin, hitMax);
	// above 1.0 the strength keeps rising, but the lobe stays within RCAS's stable limit
	float lobe = max(-0.1875, min(Max3(lobeRGB), 0.0) * sharpness);

	float bL = Luma(b), dL = Luma(d), eL = Luma(e), fL = Luma(f), hL = Luma(hh);
	float range = max(max(max(bL, dL), max(fL, hL)), eL) - min(min(min(bL, dL), min(fL, hL)), eL);
	float nz = 0.25 * (bL + dL + fL + hL) - eL;
	nz = saturate(abs(nz) / max(range, 1e-5));
	nz = 1.0 - 0.5 * nz;
	nz *= smoothstep(noise_floor * 0.5, noise_floor * 1.5 + 0.004, range);
	lobe *= nz;
	float3 outc = (lobe * (b + d + f + hh) + e) / (4.0 * lobe + 1.0);

	// 2) wider edge detail (5x5 Gaussian unsharp) and 3) fine texture (3x3)
	static const float g[5] = { 0.1201, 0.2339, 0.2920, 0.2339, 0.1201 }; // sigma 1.4
	float3 blur5 = 0.0, box3 = 0.0;
	float l5min = 1e9, l5max = -1e9;
	float3 mn3 = e, mx3 = e;
	[unroll] for(int y = -2; y <= 2; y++)
		[unroll] for(int x = -2; x <= 2; x++)
		{
			float3 t = src.Load(int3(clamp(p + int2(x, y), 0, m), 0)).rgb;
			blur5 += t * (g[y + 2] * g[x + 2]);
			float lt = Luma(t);
			l5min = min(l5min, lt);
			l5max = max(l5max, lt);
			if(abs(x) <= 1 && abs(y) <= 1)
			{
				mn3 = min(mn3, t);
				mx3 = max(mx3, t);
				box3 += t;
			}
		}
	box3 /= 9.0;
	float gate = smoothstep(noise_floor, 3.0 * noise_floor + 0.01, l5max - l5min);
	outc += (e - blur5) * (2.2 * sharpness) * gate;
	// texture works on luma only, so it adds detail without color noise
	float texDetail = eL - Luma(box3);
	float texGate = smoothstep(noise_floor * 0.7, noise_floor * 2.0 + 0.006, l5max - l5min);
	outc += texDetail * (2.5 * texture_boost) * texGate;

	// Anti-halo clamp, optionally relaxed for a punchier look
	float3 spread = (mx3 - mn3) * (0.35 * overshoot);
	outc = clamp(outc, mn3 - spread, mx3 + spread);

	// 4) clarity: local contrast against a large-radius blur, strongest in the midtones
	if(clarity > 0.0)
	{
		float lb = Luma(clarityBlur.SampleLevel(samp, i.uv, 0).rgb);
		float lc = Luma(outc);
		float mid = saturate(4.0 * lc * (1.0 - lc));
		outc += (lc - lb) * (0.9 * clarity) * mid;
	}
	return float4(saturate(outc), 1.0);
}
)HLSL";

// Debanding: removes the stair-step bands compression leaves in skies, fog and dark
// gradients. Compares the pixel with 4 points on a random ring; if they're all close it's a
// smooth gradient, so it takes the average and adds a tiny dither to keep it smooth on screen.
static const char *kPsDeband = R"HLSL(
Texture2D<float4> src : register(t0);
cbuffer DebandCB : register(b0) { float threshold; float radius; float seed; float grain; };
float Hash(float2 p) { p = frac(p * float2(443.897, 441.423)); p += dot(p, p.yx + 19.19); return frac((p.x + p.y) * p.x); }
float4 main(VSOut i) : SV_Target
{
	int2 p = int2(i.pos.xy);
	int w, h; src.GetDimensions(w, h);
	int2 m = int2(w - 1, h - 1);
	float3 c = src.Load(int3(p, 0)).rgb;
	float3 acc = c;
	float3 wsum = 1.0;
	[unroll] for(int k = 0; k < 2; k++)
	{
		float r = radius * (0.5 + 0.5 * Hash(i.pos.xy + seed + k * 7.31));
		float a = Hash(i.pos.xy * 1.37 + seed * 3.1 + k * 3.7) * 6.2831853;
		int2 o = int2(round(float2(cos(a), sin(a)) * r));
		float3 s0 = src.Load(int3(clamp(p + o, 0, m), 0)).rgb;
		float3 s1 = src.Load(int3(clamp(p - o, 0, m), 0)).rgb;
		int2 o2 = int2(-o.y, o.x);
		float3 s2 = src.Load(int3(clamp(p + o2, 0, m), 0)).rgb;
		float3 s3 = src.Load(int3(clamp(p - o2, 0, m), 0)).rgb;
		float3 avg = 0.25 * (s0 + s1 + s2 + s3);
		float3 diff = max(max(abs(s0 - c), abs(s1 - c)), max(abs(s2 - c), abs(s3 - c)));
		float3 wgt = saturate(1.0 - diff / threshold);
		wgt *= wgt;
		acc += avg * wgt;
		wsum += wgt;
	}
	float3 outc = acc / wsum;
	// triangular dither, about one 10-bit step, only where we smoothed
	float n = Hash(i.pos.xy + seed * 11.0) + Hash(i.pos.yx * 1.7 + seed) - 1.0;
	float smoothed = saturate((wsum.g - 1.0) * 0.5);
	outc += n * grain * smoothed;
	return float4(saturate(outc), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------
// Ambient lighting: tiny, temporally smoothed copy of the picture

// Ambient light, level 1: averages blocks of the frame in LINEAR light (nits, BT.709),
// decoding PQ or gamma first, so HDR and SDR streams produce the same, correct glow.
static const char *kCsBox8 = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer AmbDownCB : register(b0) { int decode; float paper_white_d; float2 padd; }; // decode: 0 none, 1 gamma, 2 PQ
float3 Decode(float3 c)
{
	if(decode == 2) return max(mul(BT2020_TO_BT709, PqToLinear(c) * 10000.0), 0.0);
	if(decode == 1) return pow(saturate(c), 2.2) * paper_white_d;
	return c;
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h; dst.GetDimensions(w, h);
	if(id.x >= w || id.y >= h) return;
	uint sw, sh; src.GetDimensions(sw, sh);
	float2 scale = float2(sw, sh) / float2(w, h);
	float2 o = float2(id.xy) * scale;
	int2 m = int2(sw - 1, sh - 1);
	float3 acc = 0.0;
	[unroll] for(int y = 0; y < 4; y++)
		[unroll] for(int x = 0; x < 4; x++)
			acc += Decode(src.Load(int3(min(int2(o + (float2(x, y) + 0.5) * scale / 4.0), m), 0)).rgb);
	dst[id.xy] = float4(acc / 16.0, 1.0);
}
)HLSL";

static const char *kCsAmbientBlend = R"HLSL(
Texture2D<float4> cur : register(t0);
Texture2D<float4> prev : register(t1);
RWTexture2D<float4> dst : register(u0);
cbuffer AmbCB : register(b0) { float alpha; float3 pada; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h; dst.GetDimensions(w, h);
	if(id.x >= w || id.y >= h) return;
	int2 m = int2(w - 1, h - 1);
	int2 p = int2(id.xy);
	float3 c = cur.Load(int3(p, 0)).rgb * 0.4
		+ (cur.Load(int3(clamp(p + int2(1, 0), 0, m), 0)).rgb + cur.Load(int3(clamp(p - int2(1, 0), 0, m), 0)).rgb
		 + cur.Load(int3(clamp(p + int2(0, 1), 0, m), 0)).rgb + cur.Load(int3(clamp(p - int2(0, 1), 0, m), 0)).rgb) * 0.15;
	float3 pr = prev.Load(int3(p, 0)).rgb;
	// smooth steady scenes, but follow scene cuts / flashes quickly
	float change = abs(Luma(c) - Luma(pr)) / (Luma(c) + Luma(pr) + 5.0);
	float a = alpha >= 1.0 ? 1.0 : lerp(alpha, 0.75, saturate(change * 4.0));
	dst[id.xy] = float4(lerp(pr, c, a), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------
// Final output: places the video (fit/fill/stretch), draws ambient light in the bars,
// applies picture settings, optional split-screen comparison, and converts to the swap
// chain's color space.
//   mode 0: SDR video -> SDR output      mode 1: SDR video -> HDR10 output (optional HDR boost)
//   mode 2: HDR video -> HDR10 output    mode 3: HDR video -> SDR output (tone mapped)
static const char *kPsVideoOut = R"HLSL(
Texture2D<float4> src : register(t0);
Texture2D<float4> amb : register(t1);
Texture2D<float4> orig : register(t2);
SamplerState samp : register(s0);
cbuffer OutCB : register(b0)
{
	float4 vid_rect;      // x, y, w, h in pixels
	float4 uv_rect;       // u0, v0, u1, v1 (crop for "fill")
	float2 src_size;
	int scaler;           // 0 bilinear, 1 bicubic (Catmull-Rom)
	int mode;
	float paper_white;    // nits of SDR white on an HDR display
	float peak;           // HDR content peak (mode 3) or display peak (mode 1)
	float ambient;        // 0 = off
	float hdr_boost;      // 0..1, SDR -> HDR highlight expansion
	float brightness;
	float contrast;
	float saturation;
	float vibrance;
	float2 win_size;
	float split;          // compare: x position of the divider in pixels, < 0 = off
	int orig_mode;        // mode for the original (left) side
	int ambient_style;    // 0 glow, 1 extend
	float3 pado;
};

float3 Bicubic(float2 uv)
{
	float2 pos = uv * src_size - 0.5;
	float2 base = floor(pos);
	float2 f = pos - base;
	float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 w3 = f * f * (-0.5 + 0.5 * f);
	float wx[4] = { w0.x, w1.x, w2.x, w3.x };
	float wy[4] = { w0.y, w1.y, w2.y, w3.y };
	int2 maxc = int2(src_size) - 1;
	float3 acc = 0.0;
	float3 mn = 1e9, mx = -1e9;
	[unroll] for(int y = 0; y < 4; y++)
		[unroll] for(int x = 0; x < 4; x++)
		{
			float3 c = src.Load(int3(clamp(int2(base) + int2(x - 1, y - 1), 0, maxc), 0)).rgb;
			acc += c * (wx[x] * wy[y]);
			if(x >= 1 && x <= 2 && y >= 1 && y <= 2) { mn = min(mn, c); mx = max(mx, c); }
		}
	return clamp(acc, mn, mx);
}

float3 Picture(float3 c)
{
	c = (c - 0.5) * contrast + 0.5 + brightness;
	float l = Luma(c);
	float sat = max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
	float s = saturation * (1.0 + vibrance * (1.0 - saturate(sat * 2.0)));
	return saturate(l + (c - l) * s);
}

float3 ToneMap(float3 nits709)
{
	float3 x = max(nits709, 0.0) / paper_white;
	float m = max(max(x.r, x.g), x.b);
	if(m <= 1e-6) return 0.0;
	float wp = max(peak / paper_white, 1.0001);
	float mt = m * (1.0 + m / (wp * wp)) / (1.0 + m);
	return x * (mt / m);
}

// Video signal (gamma or PQ) -> swap chain signal
float3 EncodeVideo(float3 c, int m)
{
	if(m == 1)
	{
		float3 lin = pow(c, 2.2);
		if(hdr_boost > 0.0)
		{
			float y = Luma(lin);
			float knee = 0.55;
			if(y > knee)
			{
				float gain = 1.0 + hdr_boost * (max(peak / paper_white, 1.0) - 1.0);
				lin *= (knee + (y - knee) * gain) / y;
			}
		}
		return LinearToPq(mul(BT709_TO_BT2020, lin) * (paper_white / 10000.0));
	}
	if(m == 3)
	{
		float3 nits = mul(BT2020_TO_BT709, PqToLinear(c) * 10000.0);
		return pow(saturate(ToneMap(nits)), 1.0 / 2.2);
	}
	return c;
}

// Linear light (nits, BT.709) -> swap chain signal
float3 EncodeLinear(float3 nits)
{
	bool hdr_out = mode == 1 || mode == 2;
	if(hdr_out)
		return LinearToPq(max(mul(BT709_TO_BT2020, nits), 0.0) / 10000.0);
	return pow(saturate(nits / paper_white), 1.0 / 2.2);
}

// Ambient light, computed in linear light so it looks the same in SDR and HDR, and capped so
// that bright HDR highlights at the picture's edge never turn the bars into floodlights.
float3 Ambient(float2 p)
{
	float2 lo = vid_rect.xy, hi = vid_rect.xy + vid_rect.zw;
	float2 q = clamp(p, lo, hi);
	float2 d = p - q;
	bool side = abs(d.x) > abs(d.y);
	float barSize = side ? (p.x < lo.x ? lo.x : win_size.x - hi.x) : (p.y < lo.y ? lo.y : win_size.y - hi.y);
	barSize = max(barSize, 1.0);
	float dn = saturate(length(d) / barSize);
	float2 quv = (q - lo) / max(vid_rect.zw, 1.0);
	float2 along = side ? float2(0, 1) : float2(1, 0);
	float2 inward = side ? float2(p.x < lo.x ? 1 : -1, 0) : float2(0, p.y < lo.y ? 1 : -1);

	float3 c;
	if(ambient_style == 1)
	{
		// "Extend": a blurred mirror of the picture continues into the bar, getting softer with distance
		float2 muv = quv + inward * dn * 0.18;
		float spread = 0.015 + 0.06 * dn;
		c = amb.SampleLevel(samp, muv, 0).rgb * 0.4
			+ (amb.SampleLevel(samp, muv + along * spread, 0).rgb + amb.SampleLevel(samp, muv - along * spread, 0).rgb) * 0.3;
	}
	else
	{
		// "Glow": soft colored light from the nearest part of the picture
		quv = lerp(quv, 0.5, 0.04) + inward * 0.03;
		float spread = 0.05 + 0.15 * dn;
		c = amb.SampleLevel(samp, quv, 0).rgb * 0.36
			+ (amb.SampleLevel(samp, quv + along * spread, 0).rgb + amb.SampleLevel(samp, quv - along * spread, 0).rgb) * 0.22
			+ (amb.SampleLevel(samp, quv + along * spread * 2.0, 0).rgb + amb.SampleLevel(samp, quv - along * spread * 2.0, 0).rgb) * 0.10;
		float l = Luma(c);
		c = max(l + (c - l) * 1.3, 0.0);
	}
	// brightness cap: soft-knee at ~45% of SDR white, scaled by intensity
	float cap = paper_white * 0.45 * ambient;
	float l = Luma(c);
	c *= (l > 1e-4) ? (cap * l / (l + cap)) / l : 0.0;
	float fall = 1.0 - smoothstep(0.0, 1.0, dn);
	return c * fall * fall * (ambient_style == 1 ? 1.4 : 1.0);
}

float4 main(VSOut i) : SV_Target
{
	float2 p = i.pos.xy;
	float2 rel = (p - vid_rect.xy) / vid_rect.zw;
	if(all(rel >= 0.0) && all(rel <= 1.0))
	{
		float2 uv = lerp(uv_rect.xy, uv_rect.zw, rel);
		if(split >= 0.0)
		{
			// divider line (hidden when it sits at the edge, i.e. fully original / fully enhanced)
			if(abs(p.x - split) < 1.5 && split > vid_rect.x + 2.0 && split < vid_rect.x + vid_rect.z - 2.0)
				return float4(EncodeLinear(float3(1, 1, 1) * paper_white * 0.8), 1.0);
			if(p.x < split)
				return float4(EncodeVideo(Picture(orig.SampleLevel(samp, uv, 0).rgb), orig_mode), 1.0);
		}
		float3 c = scaler == 1 ? Bicubic(uv) : src.SampleLevel(samp, uv, 0).rgb;
		return float4(EncodeVideo(Picture(c), mode), 1.0);
	}
	if(ambient > 0.0)
		return float4(EncodeLinear(Ambient(p)), 1.0);
	return float4(0, 0, 0, 1);
}
)HLSL";

// ---------------------------------------------------------------------------
// Frame generation: hierarchical block matching at stream resolution.
// Vectors are "half-step" at the midpoint between frames: prev(p - v) ~ cur(p + v).

static const char *kMeCB = R"HLSL(
cbuffer MeCB : register(b0)
{
	int2 grid;          // motion field size in blocks
	int block;          // block size in pixels at this level
	int radius;         // search radius in pixels at this level
	int level_scale;    // full-res pixels per pixel at this level
	float lambda;       // vector cost (penalty per pixel of deviation)
	int use_pred;       // search around pred[] (spatial predictor from the coarser level)
	int use_temporal;   // also search around temporal[] (last frame's final field)
	float2 pred_scale;  // block index -> predictor texel
	float2 temporal_scale;
};
Texture2D<float> prevL : register(t0);
Texture2D<float> curL : register(t1);
Texture2D<float4> pred : register(t2);
Texture2D<float4> temporal : register(t3);
RWTexture2D<float4> mvOut : register(u0);

float BlockSad(int2 origin, int2 v, int2 maxc)
{
	float s = 0.0;
	for(int y = 0; y < block; y++)
		for(int x = 0; x < block; x++)
		{
			int2 p = origin + int2(x, y);
			s += abs(prevL.Load(int3(clamp(p - v, 0, maxc), 0)) - curL.Load(int3(clamp(p + v, 0, maxc), 0)));
		}
	return s;
}
)HLSL";

static const char *kCsMotionSearch = R"HLSL(
void SearchAround(int2 origin, int2 center, int r, int2 maxc, inout float best, inout int2 bestV, inout float bestSad, int2 prior)
{
	for(int dy = -r; dy <= r; dy++)
		for(int dx = -r; dx <= r; dx++)
		{
			int2 v = center + int2(dx, dy);
			float sad = BlockSad(origin, v, maxc);
			int2 dev = v - prior;
			float cost = sad + lambda * float(block * block) * (abs(dev.x) + abs(dev.y)) * 0.01;
			if(cost < best)
			{
				best = cost;
				bestV = v;
				bestSad = sad;
			}
		}
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(int(id.x) >= grid.x || int(id.y) >= grid.y) return;
	uint w, h; curL.GetDimensions(w, h);
	int2 maxc = int2(w - 1, h - 1);
	int2 origin = int2(id.xy) * block;

	float best = 1e30;
	float bestSad = 0.0;
	int2 prior = 0;
	if(use_pred != 0)
		prior = int2(round(pred.Load(int3(int2((float2(id.xy) + 0.5) * pred_scale), 0)).xy / level_scale));
	int2 bestV = prior;
	SearchAround(origin, prior, radius, maxc, best, bestV, bestSad, prior);

	if(use_temporal != 0)
	{
		// Last frame's motion at this spot catches fast pans beyond the search range
		int2 tv = int2(round(temporal.Load(int3(int2((float2(id.xy) + 0.5) * temporal_scale), 0)).xy / level_scale));
		if(any(abs(tv - prior) > radius))
			SearchAround(origin, tv, 2, maxc, best, bestV, bestSad, prior);
	}

	float n = float(block * block);
	float zeroSad = BlockSad(origin, int2(0, 0), maxc);
	mvOut[id.xy] = float4(float2(bestV) * level_scale, bestSad / n, zeroSad / n);
}
)HLSL";

// Builds an in-between frame at time t (0 = previous frame, 1 = current frame)
static const char *kPsInterp = R"HLSL(
Texture2D<float4> prevF : register(t0);
Texture2D<float4> curF : register(t1);
Texture2D<float4> mv : register(t2);
SamplerState samp : register(s0);
cbuffer InterpCB : register(b0)
{
	float2 src_size;    // stream size in pixels (motion vectors are in these units)
	float2 grid_scale;  // frame uv -> motion field uv
	float t;
	float3 padi;
};
float4 main(VSOut i) : SV_Target
{
	float2 uv = i.uv;
	// Vectors live at the midpoint time; follow the motion back to where this pixel's
	// content was at the midpoint (two fixed-point steps) before sampling the field.
	float4 m = mv.SampleLevel(samp, uv * grid_scale, 0);
	float2 full = 2.0 * m.xy / src_size;
	float2 mid_uv = uv + full * (0.5 - t);
	m = mv.SampleLevel(samp, mid_uv * grid_scale, 0);
	full = 2.0 * m.xy / src_size;
	mid_uv = uv + full * (0.5 - t);
	m = mv.SampleLevel(samp, mid_uv * grid_scale, 0);
	full = 2.0 * m.xy / src_size;

	float3 a = prevF.SampleLevel(samp, uv - full * t, 0).rgb;
	float3 b = curF.SampleLevel(samp, uv + full * (1.0 - t), 0).rgb;
	float3 blended = lerp(a, b, t);

	// Where the two motion-compensated frames disagree (something appears / disappears),
	// use the one nearer in time: it is still moved along the motion, so it doesn't judder.
	float disagree = saturate((abs(Luma(a) - Luma(b)) - 0.04) / 0.10);
	float3 c = lerp(blended, t < 0.5 ? a : b, disagree);
	// Only where block matching failed completely (particles, flashes) show the unmoved real frame.
	float failed = saturate((m.z - 0.07) / 0.08);
	float3 nearest = t < 0.5 ? prevF.SampleLevel(samp, uv, 0).rgb : curF.SampleLevel(samp, uv, 0).rgb;
	return float4(lerp(c, nearest, failed), 1.0);
}
)HLSL";

static const char *kCsLumaHalf = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h; dst.GetDimensions(w, h);
	if(id.x >= w || id.y >= h) return;
	uint sw, sh; src.GetDimensions(sw, sh);
	int2 p = int2(id.xy) * 2;
	int2 m = int2(sw - 1, sh - 1);
	float l = Luma(src.Load(int3(min(p, m), 0)).rgb)
	        + Luma(src.Load(int3(min(p + int2(1, 0), m), 0)).rgb)
	        + Luma(src.Load(int3(min(p + int2(0, 1), m), 0)).rgb)
	        + Luma(src.Load(int3(min(p + int2(1, 1), m), 0)).rgb);
	dst[id.xy] = l * 0.25;
}
)HLSL";

static const char *kCsDown2 = R"HLSL(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h; dst.GetDimensions(w, h);
	if(id.x >= w || id.y >= h) return;
	uint sw, sh; src.GetDimensions(sw, sh);
	int2 p = int2(id.xy) * 2;
	int2 m = int2(sw - 1, sh - 1);
	dst[id.xy] = 0.25 * (src.Load(int3(min(p, m), 0)) + src.Load(int3(min(p + int2(1, 0), m), 0))
	                   + src.Load(int3(min(p + int2(0, 1), m), 0)) + src.Load(int3(min(p + int2(1, 1), m), 0)));
}
)HLSL";

// Repeated-frame / scene-cut detector: per 4x4 block of the quarter-res luma, counts blocks that
// changed noticeably (0 changed = the same frame again) and blocks that changed a lot.
static const char *kCsFrameDiff = R"HLSL(
Texture2D<float> la : register(t0);
Texture2D<float> lb : register(t1);
RWByteAddressBuffer counts : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h; la.GetDimensions(w, h);
	uint gw = (w + 3) / 4, gh = (h + 3) / 4;
	if(id.x >= gw || id.y >= gh) return;
	int2 m = int2(w - 1, h - 1);
	float s = 0.0;
	[unroll] for(int y = 0; y < 4; y++)
		[unroll] for(int x = 0; x < 4; x++)
		{
			int2 p = min(int2(id.xy) * 4 + int2(x, y), m);
			s += abs(la.Load(int3(p, 0)) - lb.Load(int3(p, 0)));
		}
	s /= 16.0;
	uint dummy;
	if(s > 0.012) counts.InterlockedAdd(0, 1, dummy);
	if(s > 0.12) counts.InterlockedAdd(4, 1, dummy);
}
)HLSL";

// 3x3 component-wise median filter on the motion field (removes outlier vectors)
static const char *kCsMedian = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

float Median9(float v[9])
{
	// sort the first 5 positions (selection sort) - position 4 is then the median
	[unroll] for(int i = 0; i < 5; i++)
		[unroll] for(int j = i + 1; j < 9; j++)
		{
			float lo = min(v[i], v[j]);
			v[j] = max(v[i], v[j]);
			v[i] = lo;
		}
	return v[4];
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h; src.GetDimensions(w, h);
	if(id.x >= w || id.y >= h) return;
	int2 maxc = int2(w - 1, h - 1);
	float vx[9], vy[9];
	int k = 0;
	[unroll] for(int y = -1; y <= 1; y++)
		[unroll] for(int x = -1; x <= 1; x++)
		{
			float4 m = src.Load(int3(clamp(int2(id.xy) + int2(x, y), 0, maxc), 0));
			vx[k] = m.x; vy[k] = m.y; k++;
		}
	float4 c = src.Load(int3(id.xy, 0));
	dst[id.xy] = float4(Median9(vx), Median9(vy), c.z, c.w);
}
)HLSL";

} // namespace shaders
