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

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum ffm_packet_type {
	FFM_PACKET_VIDEO,
	FFM_PACKET_AUDIO,
	FFM_PACKET_CHANGE_FILE,
};

#define FFM_SUCCESS 0
#define FFM_ERROR -1
#define FFM_UNSUPPORTED -2

/* Level tag the muxer stamps on the lines it writes to stderr. The parent reads
 * it to tell one of its own failures from the warnings FFmpeg emits routinely. */
#define FFM_LOG_ERROR "error: "
#define FFM_LOG_WARNING "warning: "

#define AVIO_BUFFER_SIZE 65536

/* Writes one warning through the same throttled, stream-key-redacting path the
 * FFmpeg log callback uses, so a per-segment failure cannot outrun the parent's
 * drain of this pipe. Implemented by the muxer executable. */
void ffm_warn(const char *format, ...);

struct ffm_packet_info {
	int64_t pts;
	int64_t dts;
	uint32_t size;
	uint32_t index;
	enum ffm_packet_type type;
	bool keyframe;
};
