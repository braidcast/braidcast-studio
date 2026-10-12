#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <inttypes.h>

#include "graphics-hook.h"
#include "dxgi-present.hpp"

#include <detours.h>

#if COMPILE_D3D12_HOOK
#include <d3d12.h>
#endif

typedef HRESULT(STDMETHODCALLTYPE *resize_buffers_t)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present_t)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present1_t)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

resize_buffers_t RealResizeBuffers = nullptr;
present_t RealPresent = nullptr;
present1_t RealPresent1 = nullptr;

thread_local int dxgi_presenting = 0;
struct ID3D12CommandQueue *dxgi_possible_swap_queues[8]{};
size_t dxgi_possible_swap_queue_count;
bool dxgi_present_attempted = false;
thread_local dxgi_phase dxgi_capture_phase = dxgi_phase::none;
thread_local dxgi_record dxgi_present_record = {};

struct dxgi_swap_data {
	IDXGISwapChain *swap;
	void (*capture)(void *, void *);
	/* Overlay mode only: runs before RealPresent, where capture cannot. */
	void (*pre_present)(IDXGISwapChain *swap);
	void (*free)(void);
	/* Whether this API holds the capture. Another API can share the swap chain
	 * (OpenGL presented through DXGI), and its presents are its own to count. */
	bool (*owns_capture)(void);
};

static struct dxgi_swap_data data = {};
static int swap_chain_mismatch_count = 0;
constexpr int swap_chain_mismtach_limit = 16;

static void reset_swap_data(void)
{
	data.swap = nullptr;
	data.capture = nullptr;
	data.pre_present = nullptr;
	data.owns_capture = nullptr;
	memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
	dxgi_possible_swap_queue_count = 0;
	dxgi_present_attempted = false;

	if (data.free) {
		data.free();
	}
	data.free = nullptr;
}

static void STDMETHODCALLTYPE SwapChainDestructed(void *pData)
{
	if (pData == data.swap) {
		reset_swap_data();
	}
}

static void init_swap_data(IDXGISwapChain *swap, void (*capture)(void *, void *), void (*pre_present)(IDXGISwapChain *),
			   void (*free)(void), bool (*owns_capture)(void))
{
	data.swap = swap;
	data.capture = capture;
	data.pre_present = pre_present;
	data.free = free;
	data.owns_capture = owns_capture;

	ID3DDestructionNotifier *notifier;
	if (SUCCEEDED(swap->QueryInterface<ID3DDestructionNotifier>(&notifier))) {
		UINT callbackID;
		notifier->RegisterDestructionCallback(&SwapChainDestructed, swap, &callbackID);
		notifier->Release();
	}
}

static bool setup_dxgi(IDXGISwapChain *swap)
{
	IUnknown *device;
	HRESULT hr;

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		ID3D11Device *d3d11 = static_cast<ID3D11Device *>(device);
		D3D_FEATURE_LEVEL level = d3d11->GetFeatureLevel();
		device->Release();

		if (level >= D3D_FEATURE_LEVEL_11_0) {
			hlog("Found D3D11 11.0 device on swap chain");

			init_swap_data(swap, d3d11_capture, nullptr, d3d11_free, d3d11_owns_capture);
			return true;
		}
	}

	hr = swap->GetDevice(__uuidof(ID3D10Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		device->Release();

		hlog("Found D3D10 device on swap chain");

		init_swap_data(swap, d3d10_capture, nullptr, d3d10_free, d3d10_owns_capture);
		return true;
	}

	hr = swap->GetDevice(__uuidof(ID3D11Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		device->Release();

		hlog("Found D3D11 device on swap chain");

		init_swap_data(swap, d3d11_capture, nullptr, d3d11_free, d3d11_owns_capture);
		return true;
	}

#if COMPILE_D3D12_HOOK
	hr = swap->GetDevice(__uuidof(ID3D12Device), (void **)&device);
	if (SUCCEEDED(hr)) {
		device->Release();

		hlog("Found D3D12 device on swap chain: swap=0x%" PRIX64 ", device=0x%" PRIX64,
		     (uint64_t)(uintptr_t)swap, (uint64_t)(uintptr_t)device);
		for (size_t i = 0; i < dxgi_possible_swap_queue_count; ++i) {
			hlog("    queue=0x%" PRIX64, (uint64_t)(uintptr_t)dxgi_possible_swap_queues[i]);
		}

		if (dxgi_possible_swap_queue_count > 0) {
			init_swap_data(swap, d3d12_capture, d3d12_pre_present, d3d12_free, d3d12_owns_capture);
			return true;
		}
	}
#endif

	hlog_verbose("Failed to setup DXGI");
	return false;
}

static bool resize_buffers_called = false;

static HRESULT STDMETHODCALLTYPE hook_resize_buffers(IDXGISwapChain *swap, UINT buffer_count, UINT width, UINT height,
						     DXGI_FORMAT format, UINT flags)
{
	hlog_verbose("ResizeBuffers callback");

	reset_swap_data();

	const HRESULT hr = RealResizeBuffers(swap, buffer_count, width, height, format, flags);

	resize_buffers_called = true;

	return hr;
}

static inline IUnknown *get_dxgi_backbuffer(IDXGISwapChain *swap)
{
	IUnknown *res = nullptr;

	const HRESULT hr = swap->GetBuffer(0, IID_PPV_ARGS(&res));
	if (FAILED(hr)) {
		hlog_hr("get_dxgi_backbuffer: GetBuffer failed", hr);
	}

	return res;
}

/* Counted here rather than in the API's capture function, so a present whose back
 * buffer could not be fetched, or whose post-overlay capture was skipped, still
 * counts once. Only the outermost present counts, and owns_capture is loaded once:
 * another swap chain's present can null it while this one is blocked in RealPresent. */
static void count_present(bool capture)
{
	bool (*const owns)() = *static_cast<bool (*volatile *)()>(&data.owns_capture);
	if (capture && dxgi_presenting == 0 && owns && owns()) {
		hook_count_present();
	}
}

static void update_mismatch_count(bool match)
{
	if (match) {
		swap_chain_mismatch_count = 0;
	} else {
		++swap_chain_mismatch_count;

		if (swap_chain_mismatch_count == swap_chain_mismtach_limit) {
			reset_swap_data();
			swap_chain_mismatch_count = 0;
		}
	}
}

/* What one Present hook call decided before RealPresent. The capture functions
 * are loaded once, after setup_dxgi: another swap chain's mismatch reset can null
 * data.capture while this Present is blocked in RealPresent. */
struct dxgi_present_call {
	bool capture;
	bool capture_overlay;
	void (*capture_fn)(void *, void *);
	void (*pre_present)(IDXGISwapChain *);
};

static dxgi_present_call begin_present_call(IDXGISwapChain *swap, bool capture_overlay, bool test_draw)
{
	dxgi_present_call call;
	call.capture_fn = *static_cast<void (*volatile *)(void *, void *)>(&data.capture);
	call.pre_present = *static_cast<void (*volatile *)(IDXGISwapChain *)>(&data.pre_present);
	call.capture = !test_draw && swap == data.swap && call.capture_fn;
	call.capture_overlay = capture_overlay;
	return call;
}

static void capture_backbuffer(const dxgi_present_call &call, IDXGISwapChain *swap, dxgi_phase phase)
{
	IUnknown *backbuffer = get_dxgi_backbuffer(swap);

	if (backbuffer) {
		dxgi_capture_phase = phase;
		call.capture_fn(swap, backbuffer);
		dxgi_capture_phase = dxgi_phase::none;
		backbuffer->Release();
	}
}

/* Overlay-mode work happens only at the outermost Present on the thread (the
 * condition count_present uses), so a Present nested in RealPresent can neither
 * record, capture nor clear the outer call's record. */
static void pre_present_capture(const dxgi_present_call &call, IDXGISwapChain *swap)
{
	if (!call.capture) {
		return;
	}

	if (!call.capture_overlay) {
		capture_backbuffer(call, swap, dxgi_phase::pre);
	} else if (dxgi_presenting == 0 && call.pre_present) {
		call.pre_present(swap);
	}
}

static void post_present_capture(const dxgi_present_call &call, IDXGISwapChain *swap, HRESULT hr)
{
	if (!call.capture || !call.capture_overlay || dxgi_presenting != 0) {
		return;
	}

	dxgi_present_record.hr = hr;

	/*
	 * It seems that the first call to Present after ResizeBuffers
	 * will cause the backbuffer to be invalidated, so do not
	 * perform the post-overlay capture if ResizeBuffers has
	 * recently been called.  (The backbuffer returned by
	 * get_dxgi_backbuffer *will* be invalid otherwise)
	 */
	if (resize_buffers_called) {
		resize_buffers_called = false;
	} else {
		capture_backbuffer(call, swap, dxgi_phase::post);
	}

	dxgi_present_record = {};
}

static HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *swap, UINT sync_interval, UINT flags)
{
	if (should_passthrough()) {
		dxgi_presenting = true;
		const HRESULT hr = RealPresent(swap, sync_interval, flags);
		dxgi_presenting = false;
		return hr;
	}

	const bool capture_overlay = global_hook_info->capture_overlay;
	const bool test_draw = (flags & DXGI_PRESENT_TEST) != 0;

	if (data.swap) {
		update_mismatch_count(swap == data.swap);
	}

	if (!data.swap && !capture_active()) {
		setup_dxgi(swap);
	}

	hlog_verbose("Present callback: sync_interval=%u, flags=%u, current_swap=0x%" PRIX64
		     ", expected_swap=0x%" PRIX64,
		     sync_interval, flags, swap, data.swap);
	const dxgi_present_call call = begin_present_call(swap, capture_overlay, test_draw);
	pre_present_capture(call, swap);

	++dxgi_presenting;
	const HRESULT hr = RealPresent(swap, sync_interval, flags);
	--dxgi_presenting;
	dxgi_present_attempted = true;

	post_present_capture(call, swap, hr);
	count_present(call.capture);

	return hr;
}

static HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1 *swap, UINT sync_interval, UINT flags,
					       const DXGI_PRESENT_PARAMETERS *params)
{
	if (should_passthrough()) {
		dxgi_presenting = true;
		const HRESULT hr = RealPresent1(swap, sync_interval, flags, params);
		dxgi_presenting = false;
		return hr;
	}

	const bool capture_overlay = global_hook_info->capture_overlay;
	const bool test_draw = (flags & DXGI_PRESENT_TEST) != 0;

	if (data.swap) {
		update_mismatch_count(swap == data.swap);
	}

	if (!data.swap && !capture_active()) {
		setup_dxgi(swap);
	}

	hlog_verbose("Present1 callback: sync_interval=%u, flags=%u, current_swap=0x%" PRIX64
		     ", expected_swap=0x%" PRIX64,
		     sync_interval, flags, swap, data.swap);
	const dxgi_present_call call = begin_present_call(swap, capture_overlay, test_draw);
	pre_present_capture(call, swap);

	++dxgi_presenting;
	const HRESULT hr = RealPresent1(swap, sync_interval, flags, params);
	--dxgi_presenting;
	dxgi_present_attempted = true;

	post_present_capture(call, swap, hr);
	count_present(call.capture);

	return hr;
}

bool hook_dxgi(void)
{
	HMODULE dxgi_module = get_system_module("dxgi.dll");
	if (!dxgi_module) {
		hlog_verbose("Failed to find dxgi.dll. Skipping hook attempt.");
		return false;
	}

	/* ---------------------- */

	void *present_addr = get_offset_addr(dxgi_module, global_hook_info->offsets.dxgi.present);
	void *resize_addr = get_offset_addr(dxgi_module, global_hook_info->offsets.dxgi.resize);
	void *present1_addr = nullptr;
	if (global_hook_info->offsets.dxgi.present1) {
		present1_addr = get_offset_addr(dxgi_module, global_hook_info->offsets.dxgi.present1);
	}

	DetourTransactionBegin();

	RealPresent = (present_t)present_addr;
	DetourAttach(&(PVOID &)RealPresent, hook_present);

	RealResizeBuffers = (resize_buffers_t)resize_addr;
	DetourAttach(&(PVOID &)RealResizeBuffers, hook_resize_buffers);

	if (present1_addr) {
		RealPresent1 = (present1_t)present1_addr;
		DetourAttach(&(PVOID &)RealPresent1, hook_present1);
	}

	const LONG error = DetourTransactionCommit();
	const bool success = error == NO_ERROR;
	if (success) {
		hlog("Hooked IDXGISwapChain::Present");
		hlog("Hooked IDXGISwapChain::ResizeBuffers");
		if (RealPresent1) {
			hlog("Hooked IDXGISwapChain1::Present1");
		}
		hlog("Hooked DXGI");
	} else {
		RealPresent = nullptr;
		RealResizeBuffers = nullptr;
		RealPresent1 = nullptr;
		hlog("Failed to attach Detours hook: %ld", error);
	}

	return success;
}
