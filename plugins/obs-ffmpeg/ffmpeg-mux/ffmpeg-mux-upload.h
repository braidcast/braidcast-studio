/*
 * Copyright (c) 2026 Braidcast
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
#include <libavformat/avformat.h>

struct hls_upload;

/* Takes over the HTTP uploads hlsenc performs, so the response to a segment can
 * be read. FFmpeg's own HTTP writer fabricates success for a write-only request
 * before anything is sent and discards the real response unparsed, so a receiver
 * that refuses a segment is indistinguishable from one that stored it: hlsenc
 * advances the media sequence and publishes the segment in the playlist either
 * way.
 *
 * Installs on `output` only when it is an HLS context uploading over HTTP, and
 * must run before avformat_write_header, which is where hlsenc copies these
 * callbacks into the per-segment context it muxes into. `settings` is the muxer
 * settings dictionary, read for the transfer deadline and the user agent.
 *
 * Returns whether the callbacks were installed. */
bool hls_upload_install(AVFormatContext *output, AVDictionary *settings);

/* The installed state, or NULL if this context was never installed on. Reads
 * without disturbing anything, because the callbacks have to stay live until the
 * context itself is gone: freeing it runs hlsenc's deinit, which can still close
 * an upload through them. Take the state before freeing the context, then hand
 * it to hls_upload_finish once the free returns. */
struct hls_upload *hls_upload_state(const AVFormatContext *output);

/* Reports what the run lost and releases the state. NULL-safe. */
void hls_upload_finish(struct hls_upload *upload);
