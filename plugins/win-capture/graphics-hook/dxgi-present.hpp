#pragma once

#include <dxgi.h>
#include <stdint.h>

#include "graphics-hook.h"

/* Which side of RealPresent the API capture function is running on. Set by the
 * DXGI Present hooks around each call, so the capture never has to infer it. */
enum class dxgi_phase { none, pre, post };

/* The outermost overlay-mode Present on this thread, as seen before RealPresent:
 * the back buffer it hands over and when. The post-Present capture checks that
 * the Present consumed exactly that buffer before it copies anything. */
struct dxgi_record {
	IDXGISwapChain *swap;
	UINT index;
	uint64_t t;
	HRESULT hr;
	bool valid;
};

extern thread_local dxgi_phase dxgi_capture_phase;
extern thread_local dxgi_record dxgi_present_record;

#if COMPILE_D3D12_HOOK
extern void d3d12_pre_present(IDXGISwapChain *swap);
#endif
