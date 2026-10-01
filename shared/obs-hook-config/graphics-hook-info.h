#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "hook-helpers.h"

#define EVENT_CAPTURE_RESTART L"CaptureHook_Restart"
#define EVENT_CAPTURE_STOP L"CaptureHook_Stop"

#define EVENT_HOOK_READY L"CaptureHook_HookReady"
#define EVENT_HOOK_EXIT L"CaptureHook_Exit"

#define EVENT_HOOK_INIT L"CaptureHook_Initialize"

#define WINDOW_HOOK_KEEPALIVE L"CaptureHook_KeepAlive"

#define MUTEX_TEXTURE1 L"CaptureHook_TextureMutex1"
#define MUTEX_TEXTURE2 L"CaptureHook_TextureMutex2"

#define SHMEM_HOOK_INFO L"CaptureHook_HookInfo"
#define SHMEM_TEXTURE L"CaptureHook_Texture"

#define PIPE_NAME "CaptureHook_Pipe"

#pragma pack(push, 8)

struct d3d8_offsets {
	uint32_t present;
};

struct d3d9_offsets {
	uint32_t present;
	uint32_t present_ex;
	uint32_t present_swap;
	uint32_t d3d9_clsoff;
	uint32_t is_d3d9ex_clsoff;
};

struct d3d12_offsets {
	uint32_t execute_command_lists;
};

struct dxgi_offsets {
	uint32_t present;
	uint32_t resize;

	uint32_t present1;
};

struct dxgi_offsets2 {
	uint32_t release;
};

struct ddraw_offsets {
	uint32_t surface_create;
	uint32_t surface_restore;
	uint32_t surface_release;
	uint32_t surface_unlock;
	uint32_t surface_blt;
	uint32_t surface_flip;
	uint32_t surface_set_palette;
	uint32_t palette_set_entries;
};

struct shmem_data {
	volatile int last_tex;
	uint32_t tex1_offset;
	uint32_t tex2_offset;
};

struct shtex_data {
	uint32_t tex_handle;
};

#define SHTEX_RING_MAGIC 0x31524742 /* 'BGR1' */
#define SHTEX_RING_MAX 8

/* Texture map layout for frame generation capture. Old readers see base only.
 * Each slot texture carries a keyed mutex (key 0) that both sides acquire with
 * a zero timeout. The hook writes the metadata with interlocked stores after
 * releasing the slot; seq is odd while a slot is being rewritten. */
struct shtex_ring {
	struct shtex_data base;
	uint32_t magic;
	uint32_t slot_count;
	uint32_t tex_handles[SHTEX_RING_MAX];
	volatile uint64_t show_ns[SHTEX_RING_MAX];
	volatile uint64_t frame_no[SHTEX_RING_MAX];
	volatile uint32_t seq[SHTEX_RING_MAX];
};
static_assert(offsetof(struct shtex_ring, base) == 0, "ABI compatibility");
static_assert(offsetof(struct shtex_ring, magic) == 4, "ABI compatibility");
static_assert(offsetof(struct shtex_ring, slot_count) == 8, "ABI compatibility");
static_assert(offsetof(struct shtex_ring, tex_handles) == 12, "ABI compatibility");
static_assert(offsetof(struct shtex_ring, show_ns) == 48, "ABI compatibility");
static_assert(offsetof(struct shtex_ring, frame_no) == 112, "ABI compatibility");
static_assert(offsetof(struct shtex_ring, seq) == 176, "ABI compatibility");
static_assert(sizeof(struct shtex_ring) == 208, "ABI compatibility");

enum capture_type {
	CAPTURE_TYPE_MEMORY,
	CAPTURE_TYPE_TEXTURE,
};

struct graphics_offsets {
	struct d3d8_offsets d3d8;
	struct d3d9_offsets d3d9;
	struct dxgi_offsets dxgi;
	struct ddraw_offsets ddraw;
	struct dxgi_offsets2 dxgi2;
	struct d3d12_offsets d3d12;
};

struct hook_info {
	/* hook version */
	uint32_t hook_ver_major;
	uint32_t hook_ver_minor;

	/* capture info */
	enum capture_type type;
	uint32_t window;
	uint32_t format;
	uint32_t cx;
	uint32_t cy;
	uint32_t UNUSED_base_cx;
	uint32_t UNUSED_base_cy;
	uint32_t pitch;
	uint32_t map_id;
	uint32_t map_size;
	bool flip;

	/* additional options */
	uint64_t frame_interval;
	bool UNUSED_use_scale;
	bool force_shmem;
	bool capture_overlay;
	bool allow_srgb_alias;

	/* hook addresses */
	struct graphics_offsets offsets;

	uint32_t reserved[119];

	/* Braidcast extensions. The hook writes bc_counters_magic once it has
	 * mapped this struct and clears it before unmapping; bc_presents and
	 * bc_frames_copied mean nothing without it, so an older hook (zero here)
	 * reads as "not counted". */
	uint32_t bc_counters_magic;
	/* OBS writes bc_magic, bc_flags and bc_canvas_interval_ns before it signals
	 * a restart, and the hook ignores them unless bc_magic matches, so an older
	 * OBS reads as all off. */
	uint32_t bc_magic;
	uint32_t bc_flags;
	uint64_t bc_canvas_interval_ns;
	/* Hook -> OBS, cumulative and wrapping; OBS takes modular deltas. Presents
	 * on the captured swap chain while the hook's capture is active, and copies
	 * made for OBS (a frame generation ring slot counts as one). */
	volatile uint32_t bc_presents;
	volatile uint32_t bc_frames_copied;
};
static_assert(offsetof(struct hook_info, reserved) == 144, "ABI compatibility");
static_assert(offsetof(struct hook_info, bc_counters_magic) == 620, "ABI compatibility");
static_assert(offsetof(struct hook_info, bc_magic) == 624, "ABI compatibility");
static_assert(offsetof(struct hook_info, bc_flags) == 628, "ABI compatibility");
static_assert(offsetof(struct hook_info, bc_canvas_interval_ns) == 632, "ABI compatibility");
static_assert(offsetof(struct hook_info, bc_presents) == 640, "ABI compatibility");
static_assert(offsetof(struct hook_info, bc_frames_copied) == 644, "ABI compatibility");
static_assert(sizeof(struct hook_info) == 648, "ABI compatibility");

#define BC_HOOK_MAGIC 0x31464342     /* 'BCF1' */
#define BC_COUNTERS_MAGIC 0x31434342 /* 'BCC1' */
#define BC_FLAG_FRAME_GEN_CAPTURE (1u << 0)

#pragma pack(pop)

/* A hook older than the frame counters leaves bc_presents and bc_frames_copied
 * at zero, which would read as a stalled capture rather than an unmeasured one. */
static inline bool hook_counts_frames(const struct hook_info *info)
{
	return info->bc_counters_magic == BC_COUNTERS_MAGIC;
}

#define GC_MAPPING_FLAGS (FILE_MAP_READ | FILE_MAP_WRITE)

static inline HANDLE create_hook_info(DWORD id)
{
	HANDLE handle = NULL;

	wchar_t new_name[64];
	const int len = swprintf(new_name, _countof(new_name), SHMEM_HOOK_INFO L"%lu", id);
	if (len > 0) {
		handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct hook_info),
					    new_name);
	}

	return handle;
}
