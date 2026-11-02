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

// Contrast adaptive sharpening (after AMD FidelityFX CAS), run at stream resolution ----
static const char *kPsCas = R"HLSL(
Texture2D<float4> src : register(t0);
cbuffer CasCB : register(b0) { float sharpness; float3 pad1; };
float4 main(VSOut i) : SV_Target
{
	int2 p = int2(i.pos.xy);
	int w, h; src.GetDimensions(w, h);
	int2 mx2 = int2(w - 1, h - 1);
	float3 a = src.Load(int3(clamp(p + int2(-1,-1), 0, mx2), 0)).rgb;
	float3 b = src.Load(int3(clamp(p + int2( 0,-1), 0, mx2), 0)).rgb;
	float3 c = src.Load(int3(clamp(p + int2( 1,-1), 0, mx2), 0)).rgb;
	float3 d = src.Load(int3(clamp(p + int2(-1, 0), 0, mx2), 0)).rgb;
	float3 e = src.Load(int3(p, 0)).rgb;
	float3 f = src.Load(int3(clamp(p + int2( 1, 0), 0, mx2), 0)).rgb;
	float3 g = src.Load(int3(clamp(p + int2(-1, 1), 0, mx2), 0)).rgb;
	float3 hh = src.Load(int3(clamp(p + int2( 0, 1), 0, mx2), 0)).rgb;
	float3 ii = src.Load(int3(clamp(p + int2( 1, 1), 0, mx2), 0)).rgb;

	float3 mnRGB = min(min(min(d, e), min(f, b)), hh);
	float3 mnRGB2 = min(mnRGB, min(min(a, c), min(g, ii)));
	mnRGB += mnRGB2;
	float3 mxRGB = max(max(max(d, e), max(f, b)), hh);
	float3 mxRGB2 = max(mxRGB, max(max(a, c), max(g, ii)));
	mxRGB += mxRGB2;

	float3 amp = saturate(min(mnRGB, 2.0 - mxRGB) / max(mxRGB, 1e-5));
	amp = sqrt(amp);
	float peak = -1.0 / lerp(8.0, 5.0, saturate(sharpness));
	float3 wRGB = amp * peak;
	float3 outc = ((b + d + f + hh) * wRGB + e) / (1.0 + 4.0 * wRGB);
	return float4(saturate(outc), 1.0);
}
)HLSL";

// Scale to the output and convert to the swap chain's color space ------------------
//   mode 0: SDR video -> SDR output      mode 1: SDR video -> HDR10 output
//   mode 2: HDR video -> HDR10 output    mode 3: HDR video -> SDR output (tone mapped)
static const char *kPsVideoOut = R"HLSL(
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer OutCB : register(b0)
{
	float2 src_size;
	int scaler;          // 0 bilinear, 1 lanczos2
	int mode;
	float paper_white;   // nits for SDR white on an HDR display
	float content_peak;  // nits, assumed mastering peak of HDR content for tone mapping
	float2 pad2;
};

float Sinc(float x) { x *= 3.14159265; return abs(x) < 1e-4 ? 1.0 : sin(x) / x; }
float Lanczos2(float x) { x = abs(x); return x < 2.0 ? Sinc(x) * Sinc(x * 0.5) : 0.0; }

float3 SampleLanczos(float2 uv)
{
	float2 pos = uv * src_size - 0.5;
	float2 base = floor(pos);
	float2 f = pos - base;
	int2 maxc = int2(src_size) - 1;
	float3 acc = 0.0;
	float wsum = 0.0;
	float3 mn = 1e9, mx = -1e9;
	[unroll] for(int y = -1; y <= 2; y++)
	{
		float wy = Lanczos2(y - f.y);
		[unroll] for(int x = -1; x <= 2; x++)
		{
			float w = Lanczos2(x - f.x) * wy;
			float3 c = src.Load(int3(clamp(int2(base) + int2(x, y), 0, maxc), 0)).rgb;
			acc += c * w;
			wsum += w;
			if(x >= 0 && x <= 1 && y >= 0 && y <= 1)
			{
				mn = min(mn, c);
				mx = max(mx, c);
			}
		}
	}
	// anti-ringing: keep the result inside the range of the 4 nearest texels
	return clamp(acc / wsum, mn, mx);
}

float3 ToneMap(float3 nits709)
{
	// Extended Reinhard on the max channel (keeps hue), SDR white = paper_white
	float3 x = max(nits709, 0.0) / paper_white;
	float m = max(max(x.r, x.g), x.b);
	if(m <= 1e-6) return 0.0;
	float wp = max(content_peak / paper_white, 1.0001);
	float mt = m * (1.0 + m / (wp * wp)) / (1.0 + m);
	return x * (mt / m);
}

float4 main(VSOut i) : SV_Target
{
	float3 c = scaler == 1 ? SampleLanczos(i.uv) : src.SampleLevel(samp, i.uv, 0).rgb;
	if(mode == 1)
	{
		float3 lin = pow(c, 2.2);
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

// UI overlay composite (premultiplied, sRGB) -----------------------------------
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
// Frame generation: hierarchical block matching + motion compensated midpoint.
// Motion vectors are "half-step" vectors v at the midpoint: prev(p - v) ~ cur(p + v).

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

// Shared by coarse search and refinement
static const char *kMeCB = R"HLSL(
cbuffer MeCB : register(b0)
{
	int2 grid;        // motion field size in blocks
	int block;        // block size in pixels at this level
	int radius;       // search radius in pixels at this level
	int level_scale;  // full-res pixels per pixel at this level
	float lambda;     // penalty per pixel of vector length (or deviation from the predictor)
	int use_pred;     // 1 = search around the vector in pred[] (refinement)
	int pad4;
};
Texture2D<float> prevL : register(t0);
Texture2D<float> curL : register(t1);
Texture2D<float4> pred : register(t2);
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
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(int(id.x) >= grid.x || int(id.y) >= grid.y) return;
	uint w, h; curL.GetDimensions(w, h);
	int2 maxc = int2(w - 1, h - 1);
	int2 origin = int2(id.xy) * block;

	int2 center = 0;
	if(use_pred != 0)
		center = int2(round(pred.Load(int3(id.xy, 0)).xy / level_scale));

	float best = 1e30;
	int2 bestV = center;
	float bestSad = 0.0;
	for(int dy = -radius; dy <= radius; dy++)
		for(int dx = -radius; dx <= radius; dx++)
		{
			int2 v = center + int2(dx, dy);
			float sad = BlockSad(origin, v, maxc);
			int2 dev = use_pred != 0 ? int2(dx, dy) : v;
			float cost = sad + lambda * float(block * block) * (abs(dev.x) + abs(dev.y)) * 0.01;
			if(cost < best)
			{
				best = cost;
				bestV = v;
				bestSad = sad;
			}
		}
	// SAD of the zero vector helps tell static areas apart from real motion
	float zeroSad = BlockSad(origin, int2(0, 0), maxc);
	float n = float(block * block);
	mvOut[id.xy] = float4(float2(bestV) * level_scale, bestSad / n, zeroSad / n);
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

// Builds the in-between frame
static const char *kPsInterp = R"HLSL(
Texture2D<float4> prevF : register(t0);
Texture2D<float4> curF : register(t1);
Texture2D<float4> mv : register(t2);
SamplerState samp : register(s0);
cbuffer InterpCB : register(b0)
{
	float2 size;        // frame size in pixels
	float2 grid_scale;  // frame uv -> motion field uv
};
float4 main(VSOut i) : SV_Target
{
	float2 uv = i.uv;
	float4 m = mv.SampleLevel(samp, uv * grid_scale, 0);
	float2 v = m.xy / size;
	float3 a = prevF.SampleLevel(samp, uv - v, 0).rgb;
	float3 b = curF.SampleLevel(samp, uv + v, 0).rgb;
	float3 cur = curF.SampleLevel(samp, uv, 0).rgb;
	float3 mid = 0.5 * (a + b);

	// Confidence: block match error and per-pixel disagreement between the two warps.
	float blockErr = m.z;
	float pixErr = abs(Luma(a) - Luma(b));
	float err = max(blockErr * 1.5, pixErr);
	float fallback = saturate((err - 0.04) / 0.10);
	// Where matching failed, show the real frame instead of a smeared guess.
	return float4(lerp(mid, cur, fallback), 1.0);
}
)HLSL";

} // namespace shaders
