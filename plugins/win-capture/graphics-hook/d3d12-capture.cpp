#include <windows.h>
#include "graphics-hook.h"

#if COMPILE_D3D12_HOOK

#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <inttypes.h>

#include <detours.h>

#include "dxgi-helpers.hpp"

#define MAX_BACKBUFFERS 8

/* Frame generation submits a real frame and its generated ones within about
 * 0.6 ms; 4 is the largest multi frame generation factor. */
constexpr uint32_t kBurstMaxPresents = 4;
constexpr uint64_t kBurstMaxNs = 2000000;
/* A burst period this long is a stall, not a display step. */
constexpr uint64_t kStepMaxPeriodNs = 100000000;

typedef HRESULT(STDMETHODCALLTYPE *PFN_ExecuteCommandLists)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);

static PFN_ExecuteCommandLists RealExecuteCommandLists = nullptr;

struct d3d12_ring {
	struct shtex_ring *info;
	ID3D11Texture2D *tex[SHTEX_RING_MAX];
	IDXGIKeyedMutex *mutex[SHTEX_RING_MAX];
	uint32_t count;
	uint32_t next_slot;
	uint64_t frame_no;
	uint64_t last_stamp;

	uint64_t canvas_interval;
	uint64_t last_bucket;
	bool have_bucket;

	uint64_t burst_start;
	uint32_t burst_len;
	uint32_t last_burst_len;
	uint64_t step;

	volatile LONG presenting;
	bool overlap_logged;
};

struct d3d12_data {
	uint32_t cx;
	uint32_t cy;
	DXGI_FORMAT format;
	bool using_shtex;
	bool multisampled;
	bool dxgi_1_4;

	ID3D11Device *device11;
	ID3D11DeviceContext *context11;
	ID3D11On12Device *device11on12;

	union {
		struct {
			struct shtex_data *shtex_info;
			UINT backbuffer_count;
			UINT cur_backbuffer;
			ID3D11Texture2D *copy_tex;
			HANDLE handle;
		};
	};

	struct d3d12_ring ring;
};

static struct d3d12_data data = {};

extern thread_local int dxgi_presenting;
extern ID3D12CommandQueue *dxgi_possible_swap_queues[8];
extern size_t dxgi_possible_swap_queue_count;
extern bool dxgi_present_attempted;

static void d3d12_ring_free(void)
{
	struct d3d12_ring &ring = data.ring;

	for (UINT i = 0; i < SHTEX_RING_MAX; i++) {
		if (ring.mutex[i]) {
			ring.mutex[i]->Release();
		}
		if (ring.tex[i]) {
			ring.tex[i]->Release();
		}
	}

	memset(&ring, 0, sizeof(ring));
}

void d3d12_free(void)
{
	d3d12_ring_free();

	if (data.copy_tex) {
		data.copy_tex->Release();
	}
	if (data.device11) {
		data.device11->Release();
	}
	if (data.context11) {
		data.context11->Release();
	}
	if (data.device11on12) {
		data.device11on12->Release();
	}

	capture_free();

	memset(&data, 0, sizeof(data));

	hlog("----------------- d3d12 capture freed ----------------");
}

static bool create_d3d12_shared_tex(UINT misc_flags, ID3D11Texture2D **tex, HANDLE *handle)
{
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc11 = {};
	desc11.Width = data.cx;
	desc11.Height = data.cy;
	desc11.MipLevels = 1;
	desc11.ArraySize = 1;
	desc11.Format = apply_dxgi_format_typeless(data.format, global_hook_info->allow_srgb_alias);
	desc11.SampleDesc.Count = 1;
	desc11.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc11.MiscFlags = misc_flags;

	hr = data.device11->CreateTexture2D(&desc11, nullptr, tex);
	if (FAILED(hr)) {
		hlog_hr("create_d3d12_shared_tex: creation of d3d11 copy tex failed", hr);
		return false;
	}

	IDXGIResource *dxgi_res;
	hr = (*tex)->QueryInterface(&dxgi_res);
	if (FAILED(hr)) {
		hlog_hr("create_d3d12_shared_tex: failed to query "
			"IDXGIResource interface from texture",
			hr);
		(*tex)->Release();
		*tex = nullptr;
		return false;
	}

	hr = dxgi_res->GetSharedHandle(handle);
	dxgi_res->Release();
	if (FAILED(hr)) {
		hlog_hr("create_d3d12_shared_tex: failed to get shared handle", hr);
		(*tex)->Release();
		*tex = nullptr;
		return false;
	}

	return true;
}

static bool create_d3d12_tex(UINT count)
{
	if (count == 0) {
		return false;
	}

	data.backbuffer_count = count;

	return create_d3d12_shared_tex(D3D11_RESOURCE_MISC_SHARED, &data.copy_tex, &data.handle);
}

enum class ring_init_result { ok, fallback, failed };

static ring_init_result d3d12_ring_init(HWND window, UINT count)
{
	struct d3d12_ring &ring = data.ring;
	uintptr_t handles[SHTEX_RING_MAX] = {};
	UINT made = 0;

	for (; made < SHTEX_RING_MAX; made++) {
		HANDLE handle;
		if (!create_d3d12_shared_tex(D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, &ring.tex[made], &handle)) {
			break;
		}

		HRESULT hr = ring.tex[made]->QueryInterface(&ring.mutex[made]);
		if (FAILED(hr)) {
			hlog_hr("d3d12_ring_init: failed to query IDXGIKeyedMutex", hr);
			ring.tex[made]->Release();
			ring.tex[made] = nullptr;
			break;
		}

		/* Nothing else can hold a slot yet, so a failure here means the
		 * keyed mutex does not work on this device. */
		hr = ring.mutex[made]->AcquireSync(0, 0);
		if (hr == S_OK) {
			hr = ring.mutex[made]->ReleaseSync(0);
		} else if (SUCCEEDED(hr)) {
			hr = E_FAIL;
		}
		if (FAILED(hr)) {
			hlog_hr("d3d12_ring_init: keyed mutex unusable", hr);
			ring.mutex[made]->Release();
			ring.mutex[made] = nullptr;
			ring.tex[made]->Release();
			ring.tex[made] = nullptr;
			break;
		}

		handles[made] = (uintptr_t)handle;
	}

	if (made < SHTEX_RING_MAX) {
		char reason[64];
		snprintf(reason, sizeof(reason), "created %u of %u slot textures", made, (UINT)SHTEX_RING_MAX);
		log_frame_gen_capture_fallback(reason);
		d3d12_ring_free();
		return ring_init_result::fallback;
	}

	data.backbuffer_count = count;
	ring.count = SHTEX_RING_MAX;
	ring.canvas_interval = global_hook_info->bc_canvas_interval_ns;

	if (!capture_init_shtex_ring(&ring.info, window, data.cx, data.cy, data.format, false, handles, ring.count)) {
		return ring_init_result::failed;
	}

	return ring_init_result::ok;
}

static bool d3d12_init_11on12(ID3D12Device *device)
{
	static HMODULE d3d11 = nullptr;
	static PFN_D3D11ON12_CREATE_DEVICE create_11_on_12 = nullptr;
	static bool initialized_11 = false;
	static bool initialized_func = false;

	if (!initialized_11 && !d3d11) {
		d3d11 = load_system_library("d3d11.dll");
		if (!d3d11) {
			hlog("d3d12_init_11on12: failed to load d3d11");
		}
		initialized_11 = true;
	}

	if (!d3d11) {
		return false;
	}

	if (!initialized_func && !create_11_on_12) {
		create_11_on_12 = (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(d3d11, "D3D11On12CreateDevice");
		if (!create_11_on_12) {
			hlog("d3d12_init_11on12: Failed to get "
			     "D3D11On12CreateDevice address");
		}

		initialized_func = true;
	}

	if (!create_11_on_12) {
		return false;
	}

	bool created = false;

	for (size_t i = 0; i < dxgi_possible_swap_queue_count; ++i) {
		hlog("d3d12_init_11on12: creating 11 device: queue=0x%" PRIX64,
		     (uint64_t)(uintptr_t)dxgi_possible_swap_queues[i]);
		IUnknown *const queue = dxgi_possible_swap_queues[i];
		const HRESULT hr =
			create_11_on_12(device, 0, nullptr, 0, &queue, 1, 0, &data.device11, &data.context11, nullptr);
		created = SUCCEEDED(hr);
		if (created) {
			break;
		}

		hlog_hr("d3d12_init_11on12: failed to create 11 device", hr);
	}

	if (!created) {
		return false;
	}

	memset(dxgi_possible_swap_queues, 0, sizeof(dxgi_possible_swap_queues));
	dxgi_possible_swap_queue_count = 0;
	dxgi_present_attempted = false;

	const HRESULT hr = data.device11->QueryInterface(IID_PPV_ARGS(&data.device11on12));
	if (FAILED(hr)) {
		hlog_hr("d3d12_init_11on12: failed to query 11on12 device", hr);
		return false;
	}

	return true;
}

static bool d3d12_shtex_init(ID3D12Device *device, HWND window, UINT count)
{
	if (!d3d12_init_11on12(device)) {
		return false;
	}

	if (count > 0 && frame_gen_capture_requested()) {
		if (global_hook_info->capture_overlay) {
			log_frame_gen_capture_fallback("third-party overlay capture is on");
		} else if (!global_hook_info->bc_canvas_interval_ns) {
			log_frame_gen_capture_fallback("the canvas frame interval is unknown");
		} else {
			switch (d3d12_ring_init(window, count)) {
			case ring_init_result::ok:
				hlog("d3d12 frame generation capture successful: %u slots", data.ring.count);
				return true;
			case ring_init_result::failed:
				return false;
			case ring_init_result::fallback:
				break;
			}
		}
	}

	if (!create_d3d12_tex(count)) {
		return false;
	}
	if (!capture_init_shtex(&data.shtex_info, window, data.cx, data.cy, data.format, false,
				(uintptr_t)data.handle)) {
		return false;
	}

	hlog("d3d12 shared texture capture successful");
	return true;
}

static inline UINT d3d12_init_format(IDXGISwapChain *swap, HWND &window)
{
	DXGI_SWAP_CHAIN_DESC desc;
	IDXGISwapChain3 *swap3;
	HRESULT hr;

	hr = swap->GetDesc(&desc);
	if (FAILED(hr)) {
		hlog_hr("d3d12_init_format: swap->GetDesc failed", hr);
		return 0;
	}

	print_swap_desc(&desc);

	data.format = strip_dxgi_format_srgb(desc.BufferDesc.Format);
	data.multisampled = desc.SampleDesc.Count > 1;
	window = desc.OutputWindow;
	data.cx = desc.BufferDesc.Width;
	data.cy = desc.BufferDesc.Height;

	hr = swap->QueryInterface(&swap3);
	if (SUCCEEDED(hr)) {
		data.dxgi_1_4 = true;
		hlog("We're DXGI1.4 boys!");
		swap3->Release();
	}

	UINT count = desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD ? 1 : desc.BufferCount;

	if (count == 1) {
		data.dxgi_1_4 = false;
	}

	if (count > MAX_BACKBUFFERS) {
		hlog("Somehow it's using more than the max backbuffers.  "
		     "Not sure why anyone would do that.");
		count = 1;
		data.dxgi_1_4 = false;
	}

	return count;
}

static void d3d12_init(IDXGISwapChain *swap)
{
	ID3D12Device *device = nullptr;
	const HRESULT hr = swap->GetDevice(IID_PPV_ARGS(&device));
	if (SUCCEEDED(hr)) {
		hlog("d3d12_init: device=0x%" PRIX64, (uint64_t)(uintptr_t)device);

		HWND window;
		UINT count = d3d12_init_format(swap, window);
		if (count > 0) {
			if (global_hook_info->force_shmem) {
				hlog("d3d12_init: shared memory capture currently "
				     "unsupported; ignoring");
			}

			if (!d3d12_shtex_init(device, window, count)) {
				d3d12_free();
			}
		}

		device->Release();
	} else {
		hlog_hr("d3d12_init: failed to get device from swap", hr);
	}
}

static inline void d3d12_copy_texture(ID3D11Resource *dst, ID3D11Resource *src)
{
	if (data.multisampled) {
		data.context11->ResolveSubresource(dst, 0, src, 0, data.format);
	} else {
		data.context11->CopyResource(dst, src);
	}
}

static inline UINT d3d12_backbuffer_index(IDXGISwapChain *swap)
{
	if (data.dxgi_1_4) {
		IDXGISwapChain3 *swap3 = reinterpret_cast<IDXGISwapChain3 *>(swap);
		return swap3->GetCurrentBackBufferIndex();
	}

	return data.cur_backbuffer;
}

static inline void d3d12_advance_backbuffer(void)
{
	if (!data.dxgi_1_4) {
		if (++data.cur_backbuffer >= data.backbuffer_count) {
			data.cur_backbuffer = 0;
		}
	}
}

/* The back buffer is wrapped for this one copy and released before returning:
 * a reference held past the Present keeps the game's swap chain alive. A
 * keyed mutex held on dst is released before the flush that submits the copy. */
static bool d3d12_copy_backbuffer(IDXGISwapChain *swap, ID3D11Texture2D *dst, IDXGIKeyedMutex *dst_mutex)
{
	bool copied = false;

	ID3D12Resource *backbuffer12;
	if (SUCCEEDED(swap->GetBuffer(d3d12_backbuffer_index(swap), IID_PPV_ARGS(&backbuffer12)))) {
		D3D11_RESOURCE_FLAGS rf11 = {};
		ID3D11Resource *backbuffer;
		if (SUCCEEDED(data.device11on12->CreateWrappedResource(
			    backbuffer12, &rf11, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
			    IID_PPV_ARGS(&backbuffer)))) {
			data.device11on12->AcquireWrappedResources(&backbuffer, 1);
			d3d12_copy_texture(dst, backbuffer);
			data.device11on12->ReleaseWrappedResources(&backbuffer, 1);
			if (dst_mutex) {
				dst_mutex->ReleaseSync(0);
			}
			data.context11->Flush();

			d3d12_advance_backbuffer();

			backbuffer->Release();
			copied = true;
		}

		backbuffer12->Release();
	}

	return copied;
}

static inline void d3d12_shtex_capture(IDXGISwapChain *swap)
{
	if (!data.device11on12) {
		return;
	}

	d3d12_copy_backbuffer(swap, data.copy_tex, nullptr);
}

static bool d3d12_ring_acquire_slot(uint32_t *slot)
{
	struct d3d12_ring &ring = data.ring;

	for (uint32_t i = 0; i < ring.count; i++) {
		const uint32_t candidate = (ring.next_slot + i) % ring.count;
		if (ring.mutex[candidate]->AcquireSync(0, 0) == S_OK) {
			*slot = candidate;
			return true;
		}
	}

	return false;
}

static bool d3d12_ring_copy(IDXGISwapChain *swap, uint64_t stamp)
{
	struct d3d12_ring &ring = data.ring;

	uint32_t slot;
	if (!d3d12_ring_acquire_slot(&slot)) {
		return false;
	}

	struct shtex_ring *info = ring.info;
	InterlockedIncrement((volatile LONG *)&info->seq[slot]);

	if (!d3d12_copy_backbuffer(swap, ring.tex[slot], ring.mutex[slot])) {
		ring.mutex[slot]->ReleaseSync(0);
		InterlockedIncrement((volatile LONG *)&info->seq[slot]);
		return false;
	}

	InterlockedExchange64((volatile LONG64 *)&info->show_ns[slot], (LONG64)stamp);
	InterlockedExchange64((volatile LONG64 *)&info->frame_no[slot], (LONG64)++ring.frame_no);
	InterlockedIncrement((volatile LONG *)&info->seq[slot]);

	ring.last_stamp = stamp;
	ring.next_slot = (slot + 1) % ring.count;
	return true;
}

static void d3d12_ring_update_step(uint64_t period, uint32_t size)
{
	struct d3d12_ring &ring = data.ring;

	if (size != ring.last_burst_len || period > kStepMaxPeriodNs) {
		ring.step = 0;
		ring.last_burst_len = size;
		return;
	}

	const uint64_t sample = period / size;
	ring.step = ring.step ? (ring.step * 7 + sample) / 8 : sample;
}

static void d3d12_ring_capture(IDXGISwapChain *swap)
{
	struct d3d12_ring &ring = data.ring;

	if (!data.device11on12 || !capture_active()) {
		return;
	}

	if (InterlockedIncrement(&ring.presenting) > 1 && !ring.overlap_logged) {
		hlog("Frame generation capture: Presents overlapped on two threads");
		ring.overlap_logged = true;
	}

	const uint64_t t = os_gettime_ns();

	if (ring.burst_len == 0 || ring.burst_len >= kBurstMaxPresents || t - ring.burst_start > kBurstMaxNs) {
		if (ring.burst_len > 0) {
			d3d12_ring_update_step(t - ring.burst_start, ring.burst_len);
		}

		ring.burst_start = t;
		ring.burst_len = 0;
	}

	const uint32_t k = ring.burst_len++;

	uint64_t stamp = ring.burst_start + k * ring.step;
	if (stamp <= ring.last_stamp) {
		stamp = ring.last_stamp + 1;
	}

	/* The only rate bound the ring needs: stamps only grow, so each half canvas
	 * interval of show time gets at most one copy however fast the game presents
	 * (the legacy limiter's rate). One copy per full interval is not enough: the
	 * display's show times drift in phase against the canvas ticks, and a tick
	 * then finds no new frame (45 distinct/s at 4x on a 60 fps canvas). */
	const uint64_t bucket = stamp / (ring.canvas_interval / 2);
	if ((!ring.have_bucket || bucket > ring.last_bucket) && d3d12_ring_copy(swap, stamp)) {
		ring.last_bucket = bucket;
		ring.have_bucket = true;
	}

	InterlockedDecrement(&ring.presenting);
}

void d3d12_capture(void *swap_ptr, void *)
{
	IDXGISwapChain *swap = (IDXGISwapChain *)swap_ptr;

	if (capture_should_stop()) {
		d3d12_free();
	}
	if (capture_should_init()) {
		d3d12_init(swap);
	}
	if (data.ring.count) {
		d3d12_ring_capture(swap);
	} else if (data.handle != nullptr && capture_ready()) {
		d3d12_shtex_capture(swap);
	}
}

static bool try_append_queue_if_unique(ID3D12CommandQueue *queue)
{
	for (size_t i = 0; i < dxgi_possible_swap_queue_count; ++i) {
		if (dxgi_possible_swap_queues[i] == queue) {
			return false;
		}
	}

	dxgi_possible_swap_queues[dxgi_possible_swap_queue_count] = queue;
	++dxgi_possible_swap_queue_count;
	return true;
}

static HRESULT STDMETHODCALLTYPE hook_execute_command_lists(ID3D12CommandQueue *queue, UINT NumCommandLists,
							    ID3D12CommandList *const *ppCommandLists)
{
	hlog_verbose("ExecuteCommandLists callback: queue=0x%" PRIX64, (uint64_t)(uintptr_t)queue);

	if (dxgi_possible_swap_queue_count < _countof(dxgi_possible_swap_queues)) {
		if ((dxgi_presenting > 0) && (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)) {
			if (try_append_queue_if_unique(queue)) {
				hlog("Remembering D3D12 queue from present: queue=0x%" PRIX64,
				     (uint64_t)(uintptr_t)queue);
			}
		} else if (dxgi_present_attempted && (queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)) {
			if (try_append_queue_if_unique(queue)) {
				hlog("Remembering D3D12 queue from first direct submit after present: queue=0x%" PRIX64,
				     (uint64_t)(uintptr_t)queue);
			}
		} else {
			hlog_verbose("Ignoring D3D12 queue=0x%" PRIX64, (uint64_t)(uintptr_t)queue);
		}
	}

	return RealExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
}

static bool manually_get_d3d12_addrs(HMODULE d3d12_module, PFN_ExecuteCommandLists *execute_command_lists_addr)
{
	PFN_D3D12_CREATE_DEVICE create = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12_module, "D3D12CreateDevice");
	if (!create) {
		hlog("Failed to load D3D12CreateDevice");
		return false;
	}

	bool success = false;
	ID3D12Device *device;
	if (SUCCEEDED(create(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
		D3D12_COMMAND_QUEUE_DESC desc{};
		ID3D12CommandQueue *queue;
		HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
		success = SUCCEEDED(hr);
		if (success) {
			void **queue_vtable = *(void ***)queue;
			*execute_command_lists_addr = (PFN_ExecuteCommandLists)queue_vtable[10];

			queue->Release();
		} else {
			hlog("Failed to create D3D12 command queue");
		}

		device->Release();
	} else {
		hlog("Failed to create D3D12 device");
	}

	return success;
}

bool hook_d3d12(void)
{
	HMODULE d3d12_module = get_system_module("d3d12.dll");
	if (!d3d12_module) {
		hlog_verbose("Failed to find d3d12.dll. Skipping hook attempt.");
		return false;
	}

	PFN_ExecuteCommandLists execute_command_lists_addr = nullptr;
	if (!manually_get_d3d12_addrs(d3d12_module, &execute_command_lists_addr)) {
		hlog("Failed to get D3D12 values");
		return true;
	}

	if (!execute_command_lists_addr) {
		hlog("Invalid D3D12 values");
		return true;
	}

	DetourTransactionBegin();

	RealExecuteCommandLists = execute_command_lists_addr;
	DetourAttach(&(PVOID &)RealExecuteCommandLists, hook_execute_command_lists);

	const LONG error = DetourTransactionCommit();
	const bool success = error == NO_ERROR;
	if (success) {
		hlog("Hooked ID3D12CommandQueue::ExecuteCommandLists");
		hlog("Hooked D3D12");
	} else {
		RealExecuteCommandLists = nullptr;
		hlog("Failed to attach Detours hook: %ld", error);
	}

	return success;
}

#endif
