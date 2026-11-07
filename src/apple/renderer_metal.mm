// SPDX-License-Identifier: AGPL-3.0-only
// Metal renderer for iPadOS (compiled with ARC).
//
//  decoded frame (VideoToolbox CVPixelBuffer, zero copy; or software YUV planes)
//    -> YUV to RGB at stream size (RGBA16F)
//    -> upscaling: MetalFX spatial ("AI super resolution" on iPad) or bilinear ("Enhanced")
//    -> sharpening (contrast-adaptive, no halos)
//    -> Present: picture adjustments + HDR tone mapping into the drawable, interface on top
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreVideo/CoreVideo.h>
#import <UIKit/UIKit.h>
#import <ImageIO/ImageIO.h>
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define HAVE_METALFX 1
#endif

#include "renderer_metal.h"
#include "../log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <imgui_impl_metal.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>

static NSString *const kShaders = @R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; float2 uv; };

// A quad covering `rect` (NDC: x0, y0 top, x1, y1 bottom); uv (0,0) is the top-left.
vertex VOut vs_quad(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]])
{
	float2 c = float2(float(vid & 1), float(vid >> 1));
	VOut o;
	o.pos = float4(mix(rect.x, rect.z, c.x), mix(rect.y, rect.w, c.y), 0.0, 1.0);
	o.uv = c;
	return o;
}

struct YuvU { float4 row0; float4 row1; float4 row2; float scale; float pad0; float pad1; float pad2; }; // 64 bytes, as in C++

fragment float4 fs_yuv_bi(VOut i [[stage_in]], texture2d<float> ty [[texture(0)]], texture2d<float> tuv [[texture(1)]],
	constant YuvU &u [[buffer(0)]], sampler s [[sampler(0)]])
{
	float4 yuv = float4(ty.sample(s, i.uv).r * u.scale, tuv.sample(s, i.uv).rg * u.scale, 1.0);
	return float4(saturate(float3(dot(u.row0, yuv), dot(u.row1, yuv), dot(u.row2, yuv))), 1.0);
}

fragment float4 fs_yuv_tri(VOut i [[stage_in]], texture2d<float> ty [[texture(0)]], texture2d<float> tu [[texture(1)]],
	texture2d<float> tv [[texture(2)]], constant YuvU &u [[buffer(0)]], sampler s [[sampler(0)]])
{
	float4 yuv = float4(ty.sample(s, i.uv).r * u.scale, tu.sample(s, i.uv).r * u.scale, tv.sample(s, i.uv).r * u.scale, 1.0);
	return float4(saturate(float3(dot(u.row0, yuv), dot(u.row1, yuv), dot(u.row2, yuv))), 1.0);
}

fragment float4 fs_copy(VOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
	return t.sample(s, i.uv);
}

// Contrast-adaptive sharpening (RCAS-style): the lobe is limited so edges never ring.
struct SharpU { float amount; float pad0; float pad1; float pad2; };
fragment float4 fs_sharpen(VOut i [[stage_in]], texture2d<float> t [[texture(0)]], constant SharpU &u [[buffer(0)]])
{
	int2 size = int2(t.get_width(), t.get_height());
	int2 p = clamp(int2(i.pos.xy), int2(0), size - 1);
	float3 b = t.read(uint2(clamp(p + int2(0, -1), int2(0), size - 1))).rgb;
	float3 d = t.read(uint2(clamp(p + int2(-1, 0), int2(0), size - 1))).rgb;
	float3 e = t.read(uint2(p)).rgb;
	float3 f = t.read(uint2(clamp(p + int2(1, 0), int2(0), size - 1))).rgb;
	float3 h = t.read(uint2(clamp(p + int2(0, 1), int2(0), size - 1))).rgb;
	float3 mn = min(min(min(b, d), min(f, h)), e);
	float3 mx = max(max(max(b, d), max(f, h)), e);
	float3 hit_min = mn / (4.0 * mx + 1e-4);
	float3 hit_max = (1.0 - mx) / (4.0 * mn - 4.0 - 1e-4);
	float3 lobe3 = max(-hit_min, hit_max);
	float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * clamp(u.amount, 0.0, 2.0);
	float3 c = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
	return float4(saturate(c), 1.0);
}

// Final picture: HDR (PQ) is tone-mapped for the SDR screen, then the picture settings.
struct OutU { float brightness; float contrast; float saturation; float vibrance; int hdr_in; float paper_white; float2 pad; };

static float3 PqToLinear(float3 n)
{
	float3 p = pow(saturate(n), 1.0 / 78.84375);
	return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

fragment float4 fs_out(VOut i [[stage_in]], texture2d<float> t [[texture(0)]], constant OutU &u [[buffer(0)]],
	sampler s [[sampler(0)]])
{
	float3 c = t.sample(s, i.uv).rgb;
	if(u.hdr_in != 0)
	{
		const float3x3 bt2020_to_709 = float3x3(float3(1.6605, -0.1246, -0.0182), float3(-0.5876, 1.1329, -0.1006),
			float3(-0.0728, -0.0083, 1.1187));
		float3 lin = max(bt2020_to_709 * (PqToLinear(c) * (10000.0 / u.paper_white)), 0.0);
		// extended Reinhard: keeps mid tones, rolls highlights off to white at 4x paper white
		float L = 4.0;
		lin = lin * (1.0 + lin / (L * L)) / (1.0 + lin);
		c = pow(saturate(lin), 1.0 / 2.2);
	}
	float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
	c = (c - 0.5) * u.contrast + 0.5 + u.brightness;
	float sat = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
	c = mix(float3(luma), c, u.saturation * (1.0 + u.vibrance * (1.0 - sat)));
	return float4(saturate(c), 1.0);
}
)MSL";

struct YuvU { float row0[4], row1[4], row2[4]; float scale; float pad[3]; };
struct SharpU { float amount; float pad[3]; };
struct OutU { float brightness, contrast, saturation, vibrance; int hdr_in; float paper_white; float pad[2]; };

struct Renderer::Impl
{
	SDL_Window *window = nullptr;
	SDL_MetalView view = nullptr;
	CAMetalLayer *layer = nil;
	id<MTLDevice> dev = nil;
	id<MTLCommandQueue> queue = nil;
	id<MTLRenderPipelineState> p_yuv_bi, p_yuv_tri, p_copy, p_sharpen, p_out, p_out_shot;
	id<MTLSamplerState> samp = nil;
	CVMetalTextureCacheRef cache = nullptr;
	id<MTLTexture> imgui_dummy = nil;
	MTLRenderPassDescriptor *imgui_desc = nil;

	// video
	int video_w = 0, video_h = 0;
	bool video_hdr = false;
	uint64_t frames = 0, serial = 0;
	id<MTLTexture> rgb = nil, up = nil, sharp = nil;   // stream size, upscaled, sharpened
	id<MTLTexture> sw[3] = { nil, nil, nil };          // software decoding planes
	id<MTLTexture> shown = nil;                         // what Present draws
	std::string status = "Off";
#ifdef HAVE_METALFX
	id<MTLFXSpatialScaler> scaler = nil;
	int scaler_in_w = 0, scaler_in_h = 0, scaler_out_w = 0, scaler_out_h = 0;
#endif
	bool metalfx_ok = false;

	int px_w = 1, px_h = 1;   // drawable size in pixels
	float density = 1.0f;

	id<MTLTexture> Make(int w, int h, MTLPixelFormat fmt, MTLTextureUsage usage)
	{
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
		td.usage = usage;
		td.storageMode = MTLStorageModePrivate;
		return [dev newTextureWithDescriptor:td];
	}
	id<MTLTexture> Ensure(id<MTLTexture> t, int w, int h, MTLPixelFormat fmt, MTLTextureUsage usage)
	{
		if(t && (int)t.width == w && (int)t.height == h && t.pixelFormat == fmt)
			return t;
		return Make(w, h, fmt, usage);
	}
	id<MTLRenderPipelineState> Pipeline(id<MTLLibrary> lib, NSString *fs, MTLPixelFormat fmt)
	{
		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		pd.vertexFunction = [lib newFunctionWithName:@"vs_quad"];
		pd.fragmentFunction = [lib newFunctionWithName:fs];
		pd.colorAttachments[0].pixelFormat = fmt;
		NSError *err = nil;
		id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
		if(!p)
			LOGE("Metal pipeline %s: %s", fs.UTF8String, err.localizedDescription.UTF8String);
		return p;
	}
	// Draw `fs` over the whole target (or `rect` in NDC)
	void Pass(id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLRenderPipelineState> p, NSArray<id<MTLTexture>> *in,
		const void *uniforms, size_t usize, bool clear = false)
	{
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = target;
		rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
		float rect[4] = { -1, 1, 1, -1 };
		[enc setRenderPipelineState:p];
		[enc setVertexBytes:rect length:sizeof(rect) atIndex:0];
		for(NSUInteger k = 0; k < in.count; k++)
			[enc setFragmentTexture:in[k] atIndex:k];
		if(uniforms)
			[enc setFragmentBytes:uniforms length:usize atIndex:0];
		[enc setFragmentSamplerState:samp atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[enc endEncoding];
	}
	void VideoRect(AspectMode aspect, float out[4], float uv[4]) const
	{
		float W = (float)px_w, H = (float)px_h;
		float vw = (float)std::max(1, video_w), vh = (float)std::max(1, video_h);
		float va = vw / vh, wa = W / H;
		uv[0] = 0; uv[1] = 0; uv[2] = 1; uv[3] = 1;
		if(aspect == AspectMode::Stretch)
		{
			out[0] = 0; out[1] = 0; out[2] = W; out[3] = H;
			return;
		}
		bool fit = aspect == AspectMode::Fit;
		float w, h;
		if((wa > va) == fit)
		{
			h = H;
			w = H * va;
		}
		else
		{
			w = W;
			h = W / va;
		}
		out[0] = (W - w) * 0.5f;
		out[1] = (H - h) * 0.5f;
		out[2] = w;
		out[3] = h;
	}
};

Renderer::Renderer() : d(new Impl()) {}
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(SDL_Window *window)
{
	d->window = window;
	d->view = SDL_Metal_CreateView(window);
	if(!d->view)
	{
		LOGE("SDL_Metal_CreateView failed: %s", SDL_GetError());
		return false;
	}
	d->layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(d->view);
	d->dev = MTLCreateSystemDefaultDevice();
	if(!d->layer || !d->dev)
	{
		LOGE("No Metal device");
		return false;
	}
	d->layer.device = d->dev;
	d->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	d->layer.framebufferOnly = NO; // screenshots read it back
	d->queue = [d->dev newCommandQueue];

	NSError *err = nil;
	id<MTLLibrary> lib = [d->dev newLibraryWithSource:kShaders options:nil error:&err];
	if(!lib)
	{
		LOGE("Metal shaders: %s", err.localizedDescription.UTF8String);
		return false;
	}
	d->p_yuv_bi = d->Pipeline(lib, @"fs_yuv_bi", MTLPixelFormatRGBA16Float);
	d->p_yuv_tri = d->Pipeline(lib, @"fs_yuv_tri", MTLPixelFormatRGBA16Float);
	d->p_copy = d->Pipeline(lib, @"fs_copy", MTLPixelFormatRGBA16Float);
	d->p_sharpen = d->Pipeline(lib, @"fs_sharpen", MTLPixelFormatRGBA16Float);
	d->p_out = d->Pipeline(lib, @"fs_out", MTLPixelFormatBGRA8Unorm);
	d->p_out_shot = d->Pipeline(lib, @"fs_out", MTLPixelFormatRGBA8Unorm);
	if(!d->p_yuv_bi || !d->p_yuv_tri || !d->p_copy || !d->p_sharpen || !d->p_out || !d->p_out_shot)
		return false;

	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	d->samp = [d->dev newSamplerStateWithDescriptor:sd];

	if(CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, d->dev, nullptr, &d->cache) != kCVReturnSuccess)
		LOGW("CVMetalTextureCache unavailable: hardware frames can't be shown");

#ifdef HAVE_METALFX
	if(@available(iOS 16.0, *))
		d->metalfx_ok = [MTLFXSpatialScalerDescriptor supportsDevice:d->dev];
#endif
	LOGI("Metal: %s, MetalFX %s", d->dev.name.UTF8String, d->metalfx_ok ? "available" : "not available");

	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(window, &w, &h);
	Resize(w, h);
	return true;
}

void Renderer::Shutdown()
{
	if(!d)
		return;
	if(d->cache)
	{
		CFRelease(d->cache);
		d->cache = nullptr;
	}
	if(d->view)
	{
		SDL_Metal_DestroyView(d->view);
		d->view = nullptr;
	}
}

void Renderer::Resize(int width, int height)
{
	d->px_w = std::max(1, width);
	d->px_h = std::max(1, height);
	if(d->layer)
		d->layer.drawableSize = CGSizeMake(d->px_w, d->px_h);
	if(d->window)
	{
		float pd = SDL_GetWindowPixelDensity(d->window);
		d->density = pd > 0 ? pd : 1.0f;
	}
}

void Renderer::RefreshDisplayInfo()
{
	if(!d->window)
		return;
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(d->window, &w, &h);
	if(w != d->px_w || h != d->px_h)
		Resize(w, h);
}

int Renderer::DisplayHz() const
{
	return (int)UIScreen.mainScreen.maximumFramesPerSecond;
}

// ---------------------------------------------------------------------------
// Interface

bool Renderer::ImGuiInit()
{
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget;
	d->imgui_dummy = [d->dev newTextureWithDescriptor:td];
	d->imgui_desc = [MTLRenderPassDescriptor renderPassDescriptor];
	d->imgui_desc.colorAttachments[0].texture = d->imgui_dummy;
	return ImGui_ImplMetal_Init(d->dev);
}

void Renderer::ImGuiShutdown() { ImGui_ImplMetal_Shutdown(); }

void Renderer::ImGuiNewFrame()
{
	// the pass descriptor only tells the backend the target's pixel format
	ImGui_ImplMetal_NewFrame(d->imgui_desc);
}

ImTextureID Renderer::CreateImage(int w, int h, const uint32_t *rgba)
{
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
	td.usage = MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = [d->dev newTextureWithDescriptor:td];
	[t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:rgba bytesPerRow:(NSUInteger)w * 4];
	return (ImTextureID)(__bridge_retained void *)t;
}

void Renderer::DestroyImage(ImTextureID id)
{
	if(id)
		CFRelease((CFTypeRef)(void *)id);
}

// ---------------------------------------------------------------------------
// Video

void Renderer::ResetVideo()
{
	d->frames = 0;
	d->video_w = d->video_h = 0;
	d->shown = nil;
	d->status = "Off";
}

bool Renderer::HasVideo() const { return d->frames > 0; }
bool Renderer::VideoHdr() const { return d->video_hdr; }
int Renderer::VideoWidth() const { return d->video_w; }
int Renderer::VideoHeight() const { return d->video_h; }
int Renderer::OutputVideoWidth() const { return d->shown ? (int)d->shown.width : d->video_w; }
int Renderer::OutputVideoHeight() const { return d->shown ? (int)d->shown.height : d->video_h; }
const std::string &Renderer::UpscalerStatus() const { return d->status; }
bool Renderer::AiSuperResolutionAvailable() const { return d->metalfx_ok; }
std::string Renderer::GpuVendorName() const { return d->dev ? std::string("Apple ") + d->dev.name.UTF8String : "Apple"; }
bool Renderer::HasFrame(uint64_t serial) const { return serial == d->serial && d->frames > 0; }

// Limited-range YUV -> RGB rows (BT.709 for SDR, BT.2020 for HDR)
static void YuvMatrix(bool bt2020, bool full_range, YuvU *u)
{
	float kr = bt2020 ? 0.2627f : 0.2126f, kb = bt2020 ? 0.0593f : 0.0722f, kg = 1.0f - kr - kb;
	float ys = full_range ? 1.0f : 255.0f / 219.0f, cs = full_range ? 1.0f : 255.0f / 224.0f;
	float yo = full_range ? 0.0f : 16.0f / 255.0f;
	float rv = 2.0f * (1.0f - kr) * cs, bu = 2.0f * (1.0f - kb) * cs;
	float gu = -bu * kb / kg, gv = -rv * kr / kg;
	// rgb = ys*(y - yo) + coeffs*(c - 0.5)
	float r[4] = { ys, 0.0f, rv, -ys * yo - 0.5f * rv };
	float g[4] = { ys, gu, gv, -ys * yo - 0.5f * (gu + gv) };
	float b[4] = { ys, bu, 0.0f, -ys * yo - 0.5f * bu };
	memcpy(u->row0, r, sizeof(r));
	memcpy(u->row1, g, sizeof(g));
	memcpy(u->row2, b, sizeof(b));
}

SubmitResult Renderer::SubmitFrame(AVFrame *frame, const ProcessOptions &o)
{
	SubmitResult result;
	if(!frame || frame->width <= 0 || frame->height <= 0)
		return result;
	@autoreleasepool
	{
		int w = frame->width, h = frame->height;
		d->video_w = w;
		d->video_h = h;
		d->video_hdr = frame->color_trc == AVCOL_TRC_SMPTE2084;
		bool bt2020 = frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL || d->video_hdr;
		YuvU yu = {};
		YuvMatrix(bt2020, frame->color_range == AVCOL_RANGE_JPEG, &yu);
		yu.scale = 1.0f;

		d->rgb = d->Ensure(d->rgb, w, h, MTLPixelFormatRGBA16Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
		id<MTLCommandBuffer> cb = [d->queue commandBuffer];
		CVMetalTextureRef cvy = nullptr, cvuv = nullptr;

		if(frame->format == AV_PIX_FMT_VIDEOTOOLBOX && d->cache)
		{
			CVPixelBufferRef pb = (CVPixelBufferRef)frame->data[3];
			OSType pf = CVPixelBufferGetPixelFormatType(pb);
			bool ten = pf == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || pf == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
			MTLPixelFormat fy = ten ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
			MTLPixelFormat fuv = ten ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
			size_t w0 = CVPixelBufferGetWidthOfPlane(pb, 0), h0 = CVPixelBufferGetHeightOfPlane(pb, 0);
			size_t w1 = CVPixelBufferGetWidthOfPlane(pb, 1), h1 = CVPixelBufferGetHeightOfPlane(pb, 1);
			CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, d->cache, pb, nullptr, fy, w0, h0, 0, &cvy);
			CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, d->cache, pb, nullptr, fuv, w1, h1, 1, &cvuv);
			if(!cvy || !cvuv)
			{
				if(cvy) CFRelease(cvy);
				if(cvuv) CFRelease(cvuv);
				return result;
			}
			d->Pass(cb, d->rgb, d->p_yuv_bi, @[ CVMetalTextureGetTexture(cvy), CVMetalTextureGetTexture(cvuv) ], &yu, sizeof(yu));
		}
		else if(frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUV420P10LE)
		{
			bool ten = frame->format == AV_PIX_FMT_YUV420P10LE;
			MTLPixelFormat pf = ten ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
			for(int p = 0; p < 3; p++)
			{
				int pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h;
				if(!d->sw[p] || (int)d->sw[p].width != pw || (int)d->sw[p].height != ph || d->sw[p].pixelFormat != pf)
				{
					MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:(NSUInteger)pw
						height:(NSUInteger)ph mipmapped:NO];
					td.usage = MTLTextureUsageShaderRead;
					td.storageMode = MTLStorageModeShared;
					d->sw[p] = [d->dev newTextureWithDescriptor:td];
				}
				[d->sw[p] replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)pw, (NSUInteger)ph) mipmapLevel:0 withBytes:frame->data[p]
					bytesPerRow:(NSUInteger)frame->linesize[p]];
			}
			yu.scale = ten ? 65535.0f / 1023.0f : 1.0f; // 10-bit values sit in the low bits
			d->Pass(cb, d->rgb, d->p_yuv_tri, @[ d->sw[0], d->sw[1], d->sw[2] ], &yu, sizeof(yu));
		}
		else
		{
			LOGW("Unsupported frame format %d", frame->format);
			return result;
		}

		// Upscale to the size the picture takes on screen
		float rect[4], uv[4];
		d->VideoRect(o.aspect, rect, uv);
		int tw = (int)rect[2], th = (int)rect[3];
		if(o.target_height > 0)
		{
			th = o.target_height;
			tw = (int)std::lround((double)th * w / h);
		}
		tw = std::min(tw, 8192);
		th = std::min(th, 8192);
		bool upscaling = o.upscale != UpscaleMode::Off && th > h + 8;
		d->shown = d->rgb;
		if(upscaling)
		{
			MTLTextureUsage u = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
			d->up = d->Ensure(d->up, tw, th, MTLPixelFormatRGBA16Float, u);
			bool ai = false;
#ifdef HAVE_METALFX
			if(o.upscale == UpscaleMode::AI && d->metalfx_ok)
			{
				if(@available(iOS 16.0, *))
				{
					if(!d->scaler || d->scaler_in_w != w || d->scaler_in_h != h || d->scaler_out_w != tw || d->scaler_out_h != th)
					{
						MTLFXSpatialScalerDescriptor *sd = [MTLFXSpatialScalerDescriptor new];
						sd.inputWidth = (NSUInteger)w;
						sd.inputHeight = (NSUInteger)h;
						sd.outputWidth = (NSUInteger)tw;
						sd.outputHeight = (NSUInteger)th;
						sd.colorTextureFormat = MTLPixelFormatRGBA16Float;
						sd.outputTextureFormat = MTLPixelFormatRGBA16Float;
						sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
						d->scaler = [sd newSpatialScalerWithDevice:d->dev];
						d->scaler_in_w = w;
						d->scaler_in_h = h;
						d->scaler_out_w = tw;
						d->scaler_out_h = th;
					}
					if(d->scaler)
					{
						d->scaler.colorTexture = d->rgb;
						d->scaler.outputTexture = d->up;
						[d->scaler encodeToCommandBuffer:cb];
						ai = true;
					}
				}
			}
#endif
			if(!ai)
				d->Pass(cb, d->up, d->p_copy, @[ d->rgb ], nullptr, 0);
			float amount = ai ? o.ai_sharpness : o.sharpness;
			if(amount > 0.01f)
			{
				d->sharp = d->Ensure(d->sharp, tw, th, MTLPixelFormatRGBA16Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
				SharpU su = { std::min(amount, 2.0f), { 0, 0, 0 } };
				d->Pass(cb, d->sharp, d->p_sharpen, @[ d->up ], &su, sizeof(su));
				d->shown = d->sharp;
			}
			else
				d->shown = d->up;
			char b[160];
			snprintf(b, sizeof(b), "%s  %dx%d  sharpen %.2f", ai ? "MetalFX upscaling" : "Enhanced (bilinear + sharpening)", tw, th, amount);
			d->status = b;
			if(o.upscale == UpscaleMode::AI && !ai)
				d->status += d->metalfx_ok ? "" : "  [MetalFX unavailable on this iPad]";
		}
		else
			d->status = o.upscale == UpscaleMode::Off ? "Off" : "Not needed (screen is not larger than the stream)";

		if(cvy || cvuv)
		{
			// keep the decoder's buffers alive until the GPU has read them
			CVMetalTextureRef y = cvy, c = cvuv;
			[cb addCompletedHandler:^(id<MTLCommandBuffer>) {
				CFRelease(y);
				CFRelease(c);
			}];
		}
		[cb commit];
		d->frames++;
		result.serial = ++d->serial;
	}
	return result;
}

void Renderer::GetVideoRect(AspectMode aspect, float rect[4]) const
{
	float uv[4];
	d->VideoRect(aspect, rect, uv);
	for(int i = 0; i < 4; i++)
		rect[i] /= d->density;
}

void Renderer::Present(VideoView view, const PresentOptions &o, uint64_t)
{
	@autoreleasepool
	{
		id<CAMetalDrawable> drawable = [d->layer nextDrawable]; // waits for the display: paces the loop
		if(!drawable)
			return;
		id<MTLCommandBuffer> cb = [d->queue commandBuffer];
		MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
		rp.colorAttachments[0].texture = drawable.texture;
		rp.colorAttachments[0].loadAction = MTLLoadActionClear;
		rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
		rp.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

		if(view != VideoView::None && d->shown && d->frames > 0)
		{
			float r[4], uv[4];
			d->VideoRect(o.aspect, r, uv);
			float W = (float)d->px_w, H = (float)d->px_h;
			float ndc[4] = { r[0] / W * 2 - 1, 1 - r[1] / H * 2, (r[0] + r[2]) / W * 2 - 1, 1 - (r[1] + r[3]) / H * 2 };
			OutU ou = { o.brightness, o.contrast, o.saturation, o.vibrance, d->video_hdr ? 1 : 0, o.paper_white, { 0, 0 } };
			[enc setRenderPipelineState:d->p_out];
			[enc setVertexBytes:ndc length:sizeof(ndc) atIndex:0];
			[enc setFragmentTexture:d->shown atIndex:0];
			[enc setFragmentBytes:&ou length:sizeof(ou) atIndex:0];
			[enc setFragmentSamplerState:d->samp atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		}
		if(ImDrawData *dd = ImGui::GetDrawData())
			ImGui_ImplMetal_RenderDrawData(dd, cb, enc);
		[enc endEncoding];
		[cb presentDrawable:drawable];
		[cb commit];
	}
}

// Screenshot: the processed picture with the picture settings, as a PNG
bool Renderer::SaveScreenshot(const std::string &path_utf8, const PresentOptions &o)
{
	if(!d->shown || d->frames == 0)
		return false;
	@autoreleasepool
	{
		int w = (int)d->shown.width, h = (int)d->shown.height;
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)w
			height:(NSUInteger)h mipmapped:NO];
		td.usage = MTLTextureUsageRenderTarget;
		td.storageMode = MTLStorageModeShared;
		id<MTLTexture> out = [d->dev newTextureWithDescriptor:td];
		id<MTLCommandBuffer> cb = [d->queue commandBuffer];
		OutU ou = { o.brightness, o.contrast, o.saturation, o.vibrance, d->video_hdr ? 1 : 0, o.paper_white, { 0, 0 } };
		d->Pass(cb, out, d->p_out_shot, @[ d->shown ], &ou, sizeof(ou));
		[cb commit];
		[cb waitUntilCompleted];
		std::vector<uint8_t> px((size_t)w * h * 4);
		[out getBytes:px.data() bytesPerRow:(NSUInteger)w * 4 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0];
		CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		CGContextRef ctx = CGBitmapContextCreate(px.data(), (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
			kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
		CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : nullptr;
		bool ok = false;
		if(img)
		{
			NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path_utf8.c_str()]];
			CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
			if(dst)
			{
				CGImageDestinationAddImage(dst, img, nullptr);
				ok = CGImageDestinationFinalize(dst);
				CFRelease(dst);
			}
			CGImageRelease(img);
		}
		if(ctx)
			CGContextRelease(ctx);
		CGColorSpaceRelease(cs);
		return ok;
	}
}
