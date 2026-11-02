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

	// Gradient around the sample point (central differences on the inner 2x2, bilinear weighted)
	float2 g = 0.0;
	float wts[2][2] = { { (1 - f.x) * (1 - f.y), f.x * (1 - f.y) }, { (1 - f.x) * f.y, f.x * f.y } };
	[unroll] for(int yy = 1; yy <= 2; yy++)
		[unroll] for(int xx = 1; xx <= 2; xx++)
		{
			float2 d = float2(L[yy][xx + 1] - L[yy][xx - 1], L[yy + 1][xx] - L[yy - 1][xx]);
			g += d * wts[yy - 1][xx - 1];
		}
	float len = length(g);
	float s = saturate(len * 6.0) * edge_strength;
	float2 n = len > 1e-5 ? g / len : float2(1, 0);
	float2 t = float2(-n.y, n.x);
	float across = 1.0 + 0.7 * s; // narrower across the edge -> crisp edges
	float along = 1.0 - 0.5 * s;  // wider along the edge -> smooth, unjagged lines

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

static const char *kPsSharpen = R"HLSL(
Texture2D<float4> src : register(t0);
cbuffer SharpCB : register(b0) { float sharpness; float noise_floor; float2 pads; };
float4 main(VSOut i) : SV_Target
{
	int2 p = int2(i.pos.xy);
	int w, h; src.GetDimensions(w, h);
	int2 m = int2(w - 1, h - 1);
	float3 c = src.Load(int3(p, 0)).rgb;
	float3 n = src.Load(int3(clamp(p + int2(0, -1), 0, m), 0)).rgb;
	float3 s = src.Load(int3(clamp(p + int2(0, 1), 0, m), 0)).rgb;
	float3 e = src.Load(int3(clamp(p + int2(1, 0), 0, m), 0)).rgb;
	float3 wv = src.Load(int3(clamp(p + int2(-1, 0), 0, m), 0)).rgb;
	float lc = Luma(c), ln = Luma(n), ls = Luma(s), le = Luma(e), lw = Luma(wv);
	float lmin = min(lc, min(min(ln, ls), min(le, lw)));
	float lmax = max(lc, max(max(ln, ls), max(le, lw)));
	// Only sharpen real detail: flat/noisy areas (small local range) are left alone.
	float gate = smoothstep(noise_floor, noise_floor * 3.0 + 0.02, lmax - lmin);
	float lap = 4.0 * lc - (ln + ls + le + lw);
	float target = clamp(lc + lap * 0.35 * sharpness * gate, lmin, lmax);
	float3 outc = c + (target - lc); // luma only: no color fringes
	return float4(saturate(outc), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------
// Ambient lighting: tiny, temporally smoothed copy of the picture

static const char *kCsBox8 = R"HLSL(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint w, h; dst.GetDimensions(w, h);
	if(id.x >= w || id.y >= h) return;
	uint sw, sh; src.GetDimensions(sw, sh);
	float2 scale = float2(sw, sh) / float2(w, h);
	int2 o = int2(float2(id.xy) * scale);
	int2 m = int2(sw - 1, sh - 1);
	float3 acc = 0.0;
	[unroll] for(int y = 0; y < 8; y++)
		[unroll] for(int x = 0; x < 8; x++)
			acc += src.Load(int3(min(o + int2(float2(x, y) * scale / 8.0), m), 0)).rgb;
	dst[id.xy] = float4(acc / 64.0, 1.0);
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
	// small blur so the glow has no hard cells
	float3 c = cur.Load(int3(p, 0)).rgb * 0.4
		+ (cur.Load(int3(clamp(p + int2(1, 0), 0, m), 0)).rgb + cur.Load(int3(clamp(p - int2(1, 0), 0, m), 0)).rgb
		 + cur.Load(int3(clamp(p + int2(0, 1), 0, m), 0)).rgb + cur.Load(int3(clamp(p - int2(0, 1), 0, m), 0)).rgb) * 0.15;
	dst[id.xy] = float4(lerp(prev.Load(int3(p, 0)).rgb, c, alpha), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------
// Final output: places the video (fit/fill/stretch), draws ambient light in the bars,
// applies picture settings and converts to the swap chain's color space.
//   mode 0: SDR video -> SDR output      mode 1: SDR video -> HDR10 output (optional HDR boost)
//   mode 2: HDR video -> HDR10 output    mode 3: HDR video -> SDR output (tone mapped)
static const char *kPsVideoOut = R"HLSL(
Texture2D<float4> src : register(t0);
Texture2D<float4> amb : register(t1);
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
	float2 padw;
};

float3 Bicubic(float2 uv)
{
	float2 pos = uv * src_size - 0.5;
	float2 base = floor(pos);
	float2 f = pos - base;
	// Catmull-Rom weights
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

float3 Ambient(float2 p)
{
	float2 lo = vid_rect.xy, hi = vid_rect.xy + vid_rect.zw;
	float2 q = clamp(p, lo, hi);
	float2 d = p - q;
	bool side = abs(d.x) > abs(d.y);
	// distance into the bar, normalized by the size of the bar on that side
	float barSize = side ? (p.x < lo.x ? lo.x : win_size.x - hi.x) : (p.y < lo.y ? lo.y : win_size.y - hi.y);
	barSize = max(barSize, 1.0);
	float dn = saturate(length(d) / barSize);
	float2 quv = (q - lo) / max(vid_rect.zw, 1.0);
	quv = lerp(quv, 0.5, 0.05);
	float2 along = side ? float2(0, 1) : float2(1, 0);
	float spread = 0.05 + 0.15 * dn;
	float3 c = amb.SampleLevel(samp, quv, 0).rgb * 0.36
		+ (amb.SampleLevel(samp, quv + along * spread, 0).rgb + amb.SampleLevel(samp, quv - along * spread, 0).rgb) * 0.22
		+ (amb.SampleLevel(samp, quv + along * spread * 2.0, 0).rgb + amb.SampleLevel(samp, quv - along * spread * 2.0, 0).rgb) * 0.10;
	float l = Luma(c);
	c = saturate(l + (c - l) * 1.35); // a bit more color, like a bias light
	float fall = 1.0 - smoothstep(0.0, 1.0, dn);
	return c * ambient * fall * fall;
}

float4 main(VSOut i) : SV_Target
{
	float2 p = i.pos.xy;
	float2 rel = (p - vid_rect.xy) / vid_rect.zw;
	float3 c;
	if(all(rel >= 0.0) && all(rel <= 1.0))
	{
		float2 uv = lerp(uv_rect.xy, uv_rect.zw, rel);
		c = scaler == 1 ? Bicubic(uv) : src.SampleLevel(samp, uv, 0).rgb;
		c = Picture(c);
	}
	else if(ambient > 0.0)
		c = Ambient(p);
	else
		return float4(0, 0, 0, 1);

	if(mode == 1)
	{
		float3 lin = pow(c, 2.2);
		if(hdr_boost > 0.0)
		{
			// Expand highlights above the knee toward the display's peak brightness
			float y = Luma(lin);
			float knee = 0.55;
			if(y > knee)
			{
				float gain = 1.0 + hdr_boost * (max(peak / paper_white, 1.0) - 1.0);
				float yb = knee + (y - knee) * gain;
				lin *= yb / y;
			}
		}
		c = LinearToPq(mul(BT709_TO_BT2020, lin) * (paper_white / 10000.0));
	}
	else if(mode == 3)
	{
		float3 nits = mul(BT2020_TO_BT709, PqToLinear(c) * 10000.0);
		c = pow(saturate(ToneMap(nits)), 1.0 / 2.2);
	}
	return float4(c, 1.0);
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

	// Confidence: block match error and how much the two warps disagree at this pixel.
	float err = max(m.z * 1.5, abs(Luma(a) - Luma(b)));
	float fallback = saturate((err - 0.035) / 0.09);
	// Where matching failed (occlusions, particles), show the nearest real frame instead of a smear.
	float3 nearest = t < 0.5 ? prevF.SampleLevel(samp, uv, 0).rgb : curF.SampleLevel(samp, uv, 0).rgb;
	return float4(lerp(blended, nearest, fallback), 1.0);
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
