/*
 * Copyright (c) 2023 Lain Bailey <lain@obsproject.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "base.h"
#include "bmem.h"
#include "platform.h"
#include "threading.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#else
#include <execinfo.h>
#endif

/*
 * NOTE: totally jacked the mem alignment trick from ffmpeg, credit to them:
 *   http://www.ffmpeg.org/
 */

#define ALIGNMENT 32

/*
 * Attention, intrepid adventurers, exploring the depths of the libobs code!
 *
 * There used to be a TODO comment here saying that we should use memalign on
 * non-Windows platforms. However, since *nix/POSIX systems do not provide an
 * aligned realloc(), this is currently not (easily) achievable.
 * So while the use of posix_memalign()/memalign() would be a fairly trivial
 * change, it would also ruin our memory alignment for some reallocated memory
 * on those platforms.
 */
#if defined(_WIN32)
#define ALIGNED_MALLOC 1
#else
#define ALIGNMENT_HACK 1
#endif

static void *a_malloc(size_t size)
{
#ifdef ALIGNED_MALLOC
	return _aligned_malloc(size, ALIGNMENT);
#elif ALIGNMENT_HACK
	void *ptr = NULL;
	long diff;

	ptr = malloc(size + ALIGNMENT);
	if (ptr) {
		diff = ((~(long)ptr) & (ALIGNMENT - 1)) + 1;
		ptr = (char *)ptr + diff;
		((char *)ptr)[-1] = (char)diff;
	}

	return ptr;
#else
	return malloc(size);
#endif
}

static void *a_realloc(void *ptr, size_t size)
{
#ifdef ALIGNED_MALLOC
	return _aligned_realloc(ptr, size, ALIGNMENT);
#elif ALIGNMENT_HACK
	long diff;

	if (!ptr) {
		return a_malloc(size);
	}
	diff = ((char *)ptr)[-1];
	ptr = realloc((char *)ptr - diff, size + diff);
	if (ptr) {
		ptr = (char *)ptr + diff;
	}
	return ptr;
#else
	return realloc(ptr, size);
#endif
}

static void a_free(void *ptr)
{
#ifdef ALIGNED_MALLOC
	_aligned_free(ptr);
#elif ALIGNMENT_HACK
	if (ptr) {
		free((char *)ptr - ((char *)ptr)[-1]);
	}
#else
	free(ptr);
#endif
}

static long num_allocs = 0;

#define ALLOC_TRACK_MAX_FRAMES 24

/* Tri-state: 0 unknown, 1 disabled, 2 enabled. Resolved once from the
 * OBS_TRACK_ALLOCS env var; a benign double read of getenv is harmless. */
static long alloc_tracking = 0;

static bool alloc_tracking_enabled(void)
{
	long state = os_atomic_load_long(&alloc_tracking);
	if (state == 0) {
		const char *env = getenv("OBS_TRACK_ALLOCS");
		state = (env && strcmp(env, "1") == 0) ? 2 : 1;
		os_atomic_store_long(&alloc_tracking, state);
	}
	return state == 2;
}

struct alloc_entry {
	void *ptr;
	size_t size;
	unsigned short frame_count;
	void *frames[ALLOC_TRACK_MAX_FRAMES];
	struct alloc_entry *next;
};

/* The registry's own storage MUST use the C stdlib allocator directly, never
 * bmalloc/brealloc/bfree: routing it through the tracked allocator would make
 * every tracking operation re-enter the tracker and recurse without bound. */
static struct {
	struct alloc_entry **buckets;
	size_t bucket_count;
	size_t entry_count;
} alloc_reg = {0};

static pthread_mutex_t alloc_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline size_t alloc_hash_ptr(const void *ptr, size_t bucket_count)
{
	uintptr_t key = (uintptr_t)ptr;
	key = (key >> 4) ^ (key >> 20);
	return (size_t)key & (bucket_count - 1);
}

static void alloc_registry_grow(void)
{
	size_t new_count = alloc_reg.bucket_count ? alloc_reg.bucket_count * 2 : 1024;
	struct alloc_entry **new_buckets = calloc(new_count, sizeof(*new_buckets));
	if (!new_buckets) {
		return;
	}

	for (size_t i = 0; i < alloc_reg.bucket_count; i++) {
		struct alloc_entry *e = alloc_reg.buckets[i];
		while (e) {
			struct alloc_entry *next = e->next;
			size_t idx = alloc_hash_ptr(e->ptr, new_count);
			e->next = new_buckets[idx];
			new_buckets[idx] = e;
			e = next;
		}
	}

	free(alloc_reg.buckets);
	alloc_reg.buckets = new_buckets;
	alloc_reg.bucket_count = new_count;
}

static void alloc_registry_insert(void *ptr, size_t size, void *const *frames, unsigned short frame_count)
{
	if (alloc_reg.entry_count + 1 > (alloc_reg.bucket_count * 3) / 4) {
		alloc_registry_grow();
	}
	if (!alloc_reg.bucket_count) {
		return;
	}

	struct alloc_entry *e = malloc(sizeof(*e));
	if (!e) {
		return;
	}

	e->ptr = ptr;
	e->size = size;
	e->frame_count = frame_count;
	memcpy(e->frames, frames, (size_t)frame_count * sizeof(void *));

	size_t idx = alloc_hash_ptr(ptr, alloc_reg.bucket_count);
	e->next = alloc_reg.buckets[idx];
	alloc_reg.buckets[idx] = e;
	alloc_reg.entry_count++;
}

static void alloc_registry_remove(void *ptr)
{
	if (!alloc_reg.bucket_count) {
		return;
	}

	size_t idx = alloc_hash_ptr(ptr, alloc_reg.bucket_count);
	struct alloc_entry **link = &alloc_reg.buckets[idx];
	while (*link) {
		struct alloc_entry *e = *link;
		if (e->ptr == ptr) {
			*link = e->next;
			free(e);
			alloc_reg.entry_count--;
			return;
		}
		link = &e->next;
	}
}

static unsigned short alloc_capture_stack(void **frames)
{
#if defined(_WIN32)
	return RtlCaptureStackBackTrace(1, ALLOC_TRACK_MAX_FRAMES, frames, NULL);
#else
	int n = backtrace(frames, ALLOC_TRACK_MAX_FRAMES);
	return (unsigned short)(n < 0 ? 0 : n);
#endif
}

static void alloc_track_add(void *ptr, size_t size)
{
	void *frames[ALLOC_TRACK_MAX_FRAMES];
	unsigned short count = alloc_capture_stack(frames);
	pthread_mutex_lock(&alloc_mutex);
	alloc_registry_insert(ptr, size, frames, count);
	pthread_mutex_unlock(&alloc_mutex);
}

static void alloc_track_remove(void *ptr)
{
	pthread_mutex_lock(&alloc_mutex);
	alloc_registry_remove(ptr);
	pthread_mutex_unlock(&alloc_mutex);
}

void *bmalloc(size_t size)
{
	if (!size) {
		os_breakpoint();
		bcrash("bmalloc: Allocating 0 bytes is broken behavior, please fix your code!");
	}

	void *ptr = a_malloc(size);

	if (!ptr) {
		os_oom();
		bcrash("Out of memory while trying to allocate %lu bytes", (unsigned long)size);
	}

	os_atomic_inc_long(&num_allocs);

	if (alloc_tracking_enabled()) {
		alloc_track_add(ptr, size);
	}

	return ptr;
}

void *brealloc(void *ptr, size_t size)
{
	if (!ptr) {
		os_atomic_inc_long(&num_allocs);
	}

	if (!size) {
		os_breakpoint();
		bcrash("brealloc: Allocating 0 bytes is broken behavior, please fix your code!");
	}

	void *old_ptr = ptr;
	ptr = a_realloc(ptr, size);

	if (!ptr) {
		os_oom();
		bcrash("Out of memory while trying to allocate %lu bytes", (unsigned long)size);
	}

	if (alloc_tracking_enabled()) {
		if (old_ptr) {
			alloc_track_remove(old_ptr);
		}
		alloc_track_add(ptr, size);
	}

	return ptr;
}

void bfree(void *ptr)
{
	if (ptr) {
		os_atomic_dec_long(&num_allocs);
		if (alloc_tracking_enabled()) {
			alloc_track_remove(ptr);
		}
		a_free(ptr);
	}
}

long bnum_allocs(void)
{
	return num_allocs;
}

int base_get_alignment(void)
{
	return ALIGNMENT;
}

void *bmemdup(const void *ptr, size_t size)
{
	void *out = bmalloc(size);
	if (size) {
		memcpy(out, ptr, size);
	}

	return out;
}

/* Symbols are resolved here at dump time rather than at capture time so the
 * per-allocation fast path only stores raw frame addresses. */
void bmem_dump_outstanding(void)
{
	if (!alloc_tracking_enabled()) {
		return;
	}

	pthread_mutex_lock(&alloc_mutex);

#if defined(_WIN32)
	HANDLE process = GetCurrentProcess();
	SymSetOptions(SymGetOptions() | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize(process, NULL, TRUE);

	char sym_buf[sizeof(SYMBOL_INFO) + 256];
	SYMBOL_INFO *sym = (SYMBOL_INFO *)sym_buf;
	sym->SizeOfStruct = sizeof(SYMBOL_INFO);
	sym->MaxNameLen = 256;
#endif

	size_t leaked = 0;
	for (size_t i = 0; i < alloc_reg.bucket_count; i++) {
		for (struct alloc_entry *e = alloc_reg.buckets[i]; e; e = e->next) {
			leaked++;
			blog(LOG_WARNING, "bmem: outstanding alloc %p (%zu bytes):", e->ptr, e->size);

#if defined(_WIN32)
			for (unsigned short f = 0; f < e->frame_count; f++) {
				DWORD64 addr = (DWORD64)(uintptr_t)e->frames[f];
				DWORD64 disp = 0;

				IMAGEHLP_MODULE64 mod;
				mod.SizeOfStruct = sizeof(mod);
				const char *modname = SymGetModuleInfo64(process, addr, &mod) ? mod.ModuleName : "?";

				if (SymFromAddr(process, addr, &disp, sym)) {
					blog(LOG_WARNING, "    %s!%s+0x%llx", modname, sym->Name,
					     (unsigned long long)disp);
				} else {
					blog(LOG_WARNING, "    %s!%p", modname, e->frames[f]);
				}
			}
#else
			char **syms = backtrace_symbols(e->frames, e->frame_count);
			if (syms) {
				for (unsigned short f = 0; f < e->frame_count; f++) {
					blog(LOG_WARNING, "    %s", syms[f]);
				}
				/* backtrace_symbols returns a stdlib-malloc'd buffer */
				free(syms);
			}
#endif
		}
	}

	blog(LOG_WARNING, "bmem: %zu outstanding allocation(s)", leaked);

#if defined(_WIN32)
	SymCleanup(process);
#endif

	pthread_mutex_unlock(&alloc_mutex);
}
