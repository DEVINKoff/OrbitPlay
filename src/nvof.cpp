// SPDX-License-Identifier: AGPL-3.0-only
#include "nvof.h"
#include "log.h"

#include <windows.h>
#include <dxgi.h>

#include "../third_party/nvof/nvOpticalFlowCommon.h"

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// The few CUDA driver API declarations needed (from the public CUDA driver API; nvcuda.dll)

typedef int CUresult;
typedef int CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
typedef struct CUarray_st *CUarray;
typedef struct CUgraphicsResource_st *CUgraphicsResource;
typedef unsigned long long CUdeviceptr;
enum { CUDA_SUCCESS = 0 };
enum CUmemorytype { CU_MEMORYTYPE_HOST = 1, CU_MEMORYTYPE_DEVICE = 2, CU_MEMORYTYPE_ARRAY = 3 };
struct CUDA_MEMCPY2D
{
	size_t srcXInBytes, srcY;
	CUmemorytype srcMemoryType;
	const void *srcHost;
	CUdeviceptr srcDevice;
	CUarray srcArray;
	size_t srcPitch;
	size_t dstXInBytes, dstY;
	CUmemorytype dstMemoryType;
	void *dstHost;
	CUdeviceptr dstDevice;
	CUarray dstArray;
	size_t dstPitch;
	size_t WidthInBytes, Height;
};
#define CUAPI __stdcall
typedef CUresult(CUAPI *PFN_cuInit)(unsigned int);
typedef CUresult(CUAPI *PFN_cuD3D11GetDevice)(CUdevice *, IDXGIAdapter *);
typedef CUresult(CUAPI *PFN_cuCtxCreate)(CUcontext *, unsigned int, CUdevice);
typedef CUresult(CUAPI *PFN_cuCtxDestroy)(CUcontext);
typedef CUresult(CUAPI *PFN_cuCtxPushCurrent)(CUcontext);
typedef CUresult(CUAPI *PFN_cuCtxPopCurrent)(CUcontext *);
typedef CUresult(CUAPI *PFN_cuGraphicsD3D11RegisterResource)(CUgraphicsResource *, ID3D11Resource *, unsigned int);
typedef CUresult(CUAPI *PFN_cuGraphicsUnregisterResource)(CUgraphicsResource);
typedef CUresult(CUAPI *PFN_cuGraphicsMapResources)(unsigned int, CUgraphicsResource *, CUstream);
typedef CUresult(CUAPI *PFN_cuGraphicsUnmapResources)(unsigned int, CUgraphicsResource *, CUstream);
typedef CUresult(CUAPI *PFN_cuGraphicsSubResourceGetMappedArray)(CUarray *, CUgraphicsResource, unsigned int, unsigned int);
typedef CUresult(CUAPI *PFN_cuMemcpy2D)(const CUDA_MEMCPY2D *);

// ---------------------------------------------------------------------------
// NVOF CUDA interface (nvOpticalFlowCuda.h of the NVIDIA Optical Flow SDK, BSD license)

typedef enum
{
	NV_OF_CUDA_BUFFER_TYPE_UNDEFINED,
	NV_OF_CUDA_BUFFER_TYPE_CUARRAY,
	NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR,
} NV_OF_CUDA_BUFFER_TYPE;
typedef struct
{
	uint32_t strideXInBytes;
	uint32_t strideYInBytes;
} NV_OF_BUFFER_STRIDE;
typedef struct
{
	NV_OF_BUFFER_STRIDE strideInfo[3];
	uint32_t numPlanes;
} NV_OF_CUDA_BUFFER_STRIDE_INFO;
typedef NV_OF_STATUS(NVOFAPI *PFNNVCREATEOPTICALFLOWCUDA)(CUcontext device, NvOFHandle *hOf);
typedef NV_OF_STATUS(NVOFAPI *PFNNVOFSETIOCUDASTREAMS)(NvOFHandle hOf, CUstream inputStream, CUstream outputStream);
typedef NV_OF_STATUS(NVOFAPI *PFNNVOFCREATEGPUBUFFERCUDA)(NvOFHandle hOf, const NV_OF_BUFFER_DESCRIPTOR *bufferDesc,
	NV_OF_CUDA_BUFFER_TYPE bufferType, NvOFGPUBufferHandle *hOfGpuBuffer);
typedef CUarray(NVOFAPI *PFNNVOFGPUBUFFERGETCUARRAY)(NvOFGPUBufferHandle ofGpuBuffer);
typedef CUdeviceptr(NVOFAPI *PFNNVOFGPUBUFFERGETCUDEVICEPTR)(NvOFGPUBufferHandle ofGpuBuffer);
typedef NV_OF_STATUS(NVOFAPI *PFNVOFGPUBUFFERGETSTRIDEINFO)(NvOFGPUBufferHandle ofGpuBuffer, NV_OF_CUDA_BUFFER_STRIDE_INFO *strideInfo);
typedef NV_OF_STATUS(NVOFAPI *PFNNVOFDESTROYGPUBUFFERCUDA)(NvOFGPUBufferHandle buffer);
typedef struct
{
	PFNNVCREATEOPTICALFLOWCUDA nvCreateOpticalFlowCuda;
	PFNNVOFINIT nvOFInit;
	PFNNVOFCREATEGPUBUFFERCUDA nvOFCreateGPUBufferCuda;
	PFNNVOFGPUBUFFERGETCUARRAY nvOFGPUBufferGetCUarray;
	PFNNVOFGPUBUFFERGETCUDEVICEPTR nvOFGPUBufferGetCUdeviceptr;
	PFNVOFGPUBUFFERGETSTRIDEINFO nvOFGPUBufferGetStrideInfo;
	PFNNVOFSETIOCUDASTREAMS nvOFSetIOCudaStreams;
	PFNNVOFEXECUTE nvOFExecute;
	PFNNVOFDESTROYGPUBUFFERCUDA nvOFDestroyGPUBufferCuda;
	PFNNVOFDESTROY nvOFDestroy;
	PFNNVOFGETLASTERROR nvOFGetLastError;
	PFNNVOFGETCAPS nvOFGetCaps;
} NV_OF_CUDA_API_FUNCTION_LIST;
typedef NV_OF_STATUS(NVOFAPI *PFN_NvOFAPICreateInstanceCuda)(uint32_t apiVer, NV_OF_CUDA_API_FUNCTION_LIST *functionList);

// ---------------------------------------------------------------------------

struct HwOpticalFlow::Impl
{
	HMODULE cuda_dll = nullptr, of_dll = nullptr;
	PFN_cuInit cuInit = nullptr;
	PFN_cuD3D11GetDevice cuD3D11GetDevice = nullptr;
	PFN_cuCtxCreate cuCtxCreate = nullptr;
	PFN_cuCtxDestroy cuCtxDestroy = nullptr;
	PFN_cuCtxPushCurrent cuCtxPushCurrent = nullptr;
	PFN_cuCtxPopCurrent cuCtxPopCurrent = nullptr;
	PFN_cuGraphicsD3D11RegisterResource cuGraphicsD3D11RegisterResource = nullptr;
	PFN_cuGraphicsUnregisterResource cuGraphicsUnregisterResource = nullptr;
	PFN_cuGraphicsMapResources cuGraphicsMapResources = nullptr;
	PFN_cuGraphicsUnmapResources cuGraphicsUnmapResources = nullptr;
	PFN_cuGraphicsSubResourceGetMappedArray cuGraphicsSubResourceGetMappedArray = nullptr;
	PFN_cuMemcpy2D cuMemcpy2D = nullptr;
	NV_OF_CUDA_API_FUNCTION_LIST of = {};

	CUcontext ctx = nullptr;
	NvOFHandle hof = nullptr;
	NvOFGPUBufferHandle in[2] = {}, out = nullptr;
	uint32_t in_pitch[2] = {}, out_pitch = 0;
	std::map<ID3D11Texture2D *, CUgraphicsResource> registered;

	template<typename T> bool Load(HMODULE m, T &fn, const char *name)
	{
		fn = (T)(void *)GetProcAddress(m, name);
		if(!fn)
			LOGW("Optical flow: %s missing", name);
		return fn != nullptr;
	}
	struct Push
	{
		Impl *d;
		explicit Push(Impl *i) : d(i) { d->cuCtxPushCurrent(d->ctx); }
		~Push() { CUcontext c; d->cuCtxPopCurrent(&c); }
	};
	CUgraphicsResource Register(ID3D11Texture2D *t)
	{
		auto it = registered.find(t);
		if(it != registered.end())
			return it->second;
		CUgraphicsResource r = nullptr;
		if(cuGraphicsD3D11RegisterResource(&r, t, 0) != CUDA_SUCCESS)
			return nullptr;
		t->AddRef(); // keep the texture (and its address) alive while registered
		registered[t] = r;
		return r;
	}
	std::string LastError()
	{
		char buf[256] = {};
		uint32_t n = sizeof(buf);
		if(hof && of.nvOFGetLastError)
			of.nvOFGetLastError(hof, buf, &n);
		return buf;
	}
};

HwOpticalFlow::~HwOpticalFlow() { Shutdown(); }

bool HwOpticalFlow::Init(ID3D11Device *device, int w, int h)
{
	Shutdown();
	width = w;
	height = h;
	d = new Impl();
	auto fail = [&](const std::string &why) {
		status = why;
		LOGI("NVIDIA optical flow not used: %s", why.c_str());
		Shutdown();
		status = why; // Shutdown resets it
		return false;
	};

	d->cuda_dll = LoadLibraryW(L"nvcuda.dll");
	d->of_dll = LoadLibraryW(L"nvofapi64.dll");
	if(!d->cuda_dll || !d->of_dll)
		return fail("needs an NVIDIA GPU (RTX 20 series or newer) and its driver");
	bool ok = d->Load(d->cuda_dll, d->cuInit, "cuInit") && d->Load(d->cuda_dll, d->cuD3D11GetDevice, "cuD3D11GetDevice")
		&& d->Load(d->cuda_dll, d->cuCtxCreate, "cuCtxCreate_v2") && d->Load(d->cuda_dll, d->cuCtxDestroy, "cuCtxDestroy_v2")
		&& d->Load(d->cuda_dll, d->cuCtxPushCurrent, "cuCtxPushCurrent_v2") && d->Load(d->cuda_dll, d->cuCtxPopCurrent, "cuCtxPopCurrent_v2")
		&& d->Load(d->cuda_dll, d->cuGraphicsD3D11RegisterResource, "cuGraphicsD3D11RegisterResource")
		&& d->Load(d->cuda_dll, d->cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource")
		&& d->Load(d->cuda_dll, d->cuGraphicsMapResources, "cuGraphicsMapResources")
		&& d->Load(d->cuda_dll, d->cuGraphicsUnmapResources, "cuGraphicsUnmapResources")
		&& d->Load(d->cuda_dll, d->cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray")
		&& d->Load(d->cuda_dll, d->cuMemcpy2D, "cuMemcpy2D_v2");
	PFN_NvOFAPICreateInstanceCuda create = nullptr;
	ok = ok && d->Load(d->of_dll, create, "NvOFAPICreateInstanceCuda");
	if(!ok)
		return fail("the NVIDIA driver is too old");
	if(d->cuInit(0) != CUDA_SUCCESS)
		return fail("CUDA could not start");

	// CUDA context on the same GPU as the Direct3D device
	IDXGIDevice *dxgi = nullptr;
	IDXGIAdapter *adapter = nullptr;
	CUdevice cudev = -1;
	if(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
		d->cuD3D11GetDevice(&cudev, adapter);
	if(adapter)
		adapter->Release();
	if(dxgi)
		dxgi->Release();
	if(cudev < 0)
		return fail("the display GPU is not an NVIDIA GPU");
	if(d->cuCtxCreate(&d->ctx, 0, cudev) != CUDA_SUCCESS)
		return fail("could not create a CUDA context");
	CUcontext popped;
	d->cuCtxPopCurrent(&popped); // made current by create; we push it around each use

	if(create(NV_OF_API_VERSION, &d->of) != NV_OF_SUCCESS || !d->of.nvCreateOpticalFlowCuda)
		return fail("the driver's optical flow library is too old (update the NVIDIA driver)");
	Impl::Push push(d);
	if(d->of.nvCreateOpticalFlowCuda(d->ctx, &d->hof) != NV_OF_SUCCESS)
		return fail("this GPU has no optical flow hardware (needs RTX 20 series or newer)");

	NV_OF_INIT_PARAMS ip = {};
	ip.width = (uint32_t)w;
	ip.height = (uint32_t)h;
	ip.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
	ip.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
	ip.mode = NV_OF_MODE_OPTICALFLOW;
	ip.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
	ip.enableExternalHints = NV_OF_FALSE;
	ip.enableOutputCost = NV_OF_FALSE;
	ip.hPrivData = nullptr;
	ip.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
	ip.enableRoi = NV_OF_FALSE;
	if(d->of.nvOFInit(d->hof, &ip) != NV_OF_SUCCESS)
		return fail("optical flow init failed: " + d->LastError());

	NV_OF_BUFFER_DESCRIPTOR in_desc = { (uint32_t)w, (uint32_t)h, NV_OF_BUFFER_USAGE_INPUT, NV_OF_BUFFER_FORMAT_GRAYSCALE8 };
	NV_OF_BUFFER_DESCRIPTOR out_desc = { (uint32_t)GridWidth(), (uint32_t)GridHeight(), NV_OF_BUFFER_USAGE_OUTPUT, NV_OF_BUFFER_FORMAT_SHORT2 };
	for(int i = 0; i < 2; i++)
		if(d->of.nvOFCreateGPUBufferCuda(d->hof, &in_desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &d->in[i]) != NV_OF_SUCCESS)
			return fail("could not allocate optical flow buffers");
	if(d->of.nvOFCreateGPUBufferCuda(d->hof, &out_desc, NV_OF_CUDA_BUFFER_TYPE_CUDEVICEPTR, &d->out) != NV_OF_SUCCESS)
		return fail("could not allocate optical flow buffers");
	NV_OF_CUDA_BUFFER_STRIDE_INFO si = {};
	for(int i = 0; i < 2; i++)
	{
		d->of.nvOFGPUBufferGetStrideInfo(d->in[i], &si);
		d->in_pitch[i] = si.strideInfo[0].strideXInBytes;
	}
	d->of.nvOFGPUBufferGetStrideInfo(d->out, &si);
	d->out_pitch = si.strideInfo[0].strideXInBytes;

	ready = true;
	char b[96];
	snprintf(b, sizeof(b), "NVIDIA optical flow hardware, %dx%d", w, h);
	status = b;
	LOGI("%s", b);
	return true;
}

void HwOpticalFlow::Shutdown()
{
	ready = false;
	status = "Off";
	if(!d)
		return;
	if(d->ctx && d->cuCtxPushCurrent)
	{
		d->cuCtxPushCurrent(d->ctx);
		for(auto &kv : d->registered)
		{
			d->cuGraphicsUnregisterResource(kv.second);
			kv.first->Release();
		}
		d->registered.clear();
		for(auto &b : d->in)
			if(b)
				d->of.nvOFDestroyGPUBufferCuda(b);
		if(d->out)
			d->of.nvOFDestroyGPUBufferCuda(d->out);
		if(d->hof)
			d->of.nvOFDestroy(d->hof);
		CUcontext c;
		d->cuCtxPopCurrent(&c);
		d->cuCtxDestroy(d->ctx);
	}
	// the DLLs stay loaded (cheap, and unloading the driver's CUDA can be fragile)
	delete d;
	d = nullptr;
}

bool HwOpticalFlow::Run(ID3D11Texture2D *prev, ID3D11Texture2D *cur, ID3D11Texture2D *flow)
{
	if(!ready)
		return false;
	Impl::Push push(d);
	CUgraphicsResource res[3] = { d->Register(prev), d->Register(cur), d->Register(flow) };
	if(!res[0] || !res[1] || !res[2])
	{
		status = "could not share textures with CUDA";
		ready = false;
		return false;
	}
	// Mapping orders CUDA after the Direct3D work that wrote these textures
	if(d->cuGraphicsMapResources(3, res, nullptr) != CUDA_SUCCESS)
		return false;
	bool ok = true;
	CUarray arr[3] = {};
	for(int i = 0; i < 3 && ok; i++)
		ok = d->cuGraphicsSubResourceGetMappedArray(&arr[i], res[i], 0, 0) == CUDA_SUCCESS;

	// textures -> optical flow input buffers (GPU to GPU)
	for(int i = 0; i < 2 && ok; i++)
	{
		CUDA_MEMCPY2D c = {};
		c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
		c.srcArray = arr[i];
		c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
		c.dstDevice = d->of.nvOFGPUBufferGetCUdeviceptr(d->in[i]);
		c.dstPitch = d->in_pitch[i];
		c.WidthInBytes = (size_t)width;
		c.Height = (size_t)height;
		ok = d->cuMemcpy2D(&c) == CUDA_SUCCESS;
	}
	if(ok)
	{
		NV_OF_EXECUTE_INPUT_PARAMS in = {};
		in.inputFrame = d->in[1];      // current frame
		in.referenceFrame = d->in[0];  // previous frame
		in.disableTemporalHints = NV_OF_FALSE; // consecutive video frames: last flow helps
		NV_OF_EXECUTE_OUTPUT_PARAMS out = {};
		out.outputBuffer = d->out;
		NV_OF_STATUS st = d->of.nvOFExecute(d->hof, &in, &out);
		if(st != NV_OF_SUCCESS)
		{
			LOGW("Optical flow failed (%d): %s", (int)st, d->LastError().c_str());
			ok = false;
		}
	}
	if(ok)
	{
		// flow vectors -> the Direct3D flow texture
		CUDA_MEMCPY2D c = {};
		c.srcMemoryType = CU_MEMORYTYPE_DEVICE;
		c.srcDevice = d->of.nvOFGPUBufferGetCUdeviceptr(d->out);
		c.srcPitch = d->out_pitch;
		c.dstMemoryType = CU_MEMORYTYPE_ARRAY;
		c.dstArray = arr[2];
		c.WidthInBytes = (size_t)GridWidth() * 4; // two int16 per vector
		c.Height = (size_t)GridHeight();
		ok = d->cuMemcpy2D(&c) == CUDA_SUCCESS;
	}
	d->cuGraphicsUnmapResources(3, res, nullptr);
	return ok;
}
