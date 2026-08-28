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

#include "ffmpeg-mux-upload.h"
#include "ffmpeg-mux.h"

#include <string.h>

#include <curl/curl.h>
#include <util/c99defs.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/parseutils.h>

/* Used only when the muxer settings carry no usable deadline of their own. */
#define UPLOAD_TIMEOUT_DEFAULT_MS 8000

/* Doubled from here as the segment arrives; its final size is not knowable up
 * front, since it is whatever the keyframe interval and the bitrate make it. */
#define UPLOAD_BODY_INITIAL_SIZE 262144

struct hls_upload_counts {
	unsigned accepted;
	unsigned refused;
	unsigned failed;
	unsigned truncated;
};

/* One request, held in the AVIOContext opaque for the lifetime of the open. */
struct hls_upload_req {
	char *url;
	char *method;
	uint8_t *body;
	size_t size;
	size_t capacity;
};

struct hls_upload {
	int (*orig_io_open)(AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **options);
	int (*orig_io_close2)(AVFormatContext *s, AVIOContext *pb);

	/* Reused across every upload so the connection survives between segments,
	 * which is what hlsenc persistent mode was providing before. */
	CURL *curl;
	struct curl_slist *headers;

	struct hls_upload_counts segment;
	struct hls_upload_counts playlist;
};

/* ------------------------------------------------------------------------- */

static bool hls_upload_is_http(const char *url)
{
	return av_strstart(url, "http://", NULL) || av_strstart(url, "https://", NULL);
}

/* The playlist travels through these same callbacks and is refused for reasons
 * of its own; counting it as lost media would misstate the run. */
static struct hls_upload_counts *hls_upload_bucket(struct hls_upload *upload, const char *url, const char **kind)
{
	const bool is_playlist = strstr(url, ".m3u8") != NULL;

	*kind = is_playlist ? "playlist" : "segment";
	return is_playlist ? &upload->playlist : &upload->segment;
}

static bool hls_upload_counts_lost(const struct hls_upload_counts *counts)
{
	return counts->refused || counts->failed || counts->truncated;
}

static void hls_upload_req_free(struct hls_upload_req *req)
{
	if (!req) {
		return;
	}

	av_free(req->url);
	av_free(req->method);
	av_free(req->body);
	av_free(req);
}

static int hls_upload_write(void *opaque, const uint8_t *buf, int buf_size)
{
	struct hls_upload_req *req = opaque;

	if (buf_size <= 0) {
		return 0;
	}

	if (req->size + (size_t)buf_size > req->capacity) {
		size_t capacity = req->capacity ? req->capacity : UPLOAD_BODY_INITIAL_SIZE;
		uint8_t *body;

		while (capacity < req->size + (size_t)buf_size) {
			capacity *= 2;
		}

		body = av_realloc(req->body, capacity);
		if (!body) {
			return AVERROR(ENOMEM);
		}

		req->body = body;
		req->capacity = capacity;
	}

	memcpy(req->body + req->size, buf, (size_t)buf_size);
	req->size += (size_t)buf_size;
	return buf_size;
}

static size_t hls_upload_discard(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	UNUSED_PARAMETER(ptr);
	UNUSED_PARAMETER(userdata);
	return size * nmemb;
}

static void hls_upload_perform(struct hls_upload *upload, struct hls_upload_req *req)
{
	const char *kind;
	struct hls_upload_counts *counts = hls_upload_bucket(upload, req->url, &kind);
	long status = 0;
	CURLcode res;

	curl_easy_setopt(upload->curl, CURLOPT_URL, req->url);
	curl_easy_setopt(upload->curl, CURLOPT_CUSTOMREQUEST, req->method);
	/* The size has to be set first, or curl measures the body with strlen. */
	curl_easy_setopt(upload->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->size);
	curl_easy_setopt(upload->curl, CURLOPT_POSTFIELDS, req->body ? (const char *)req->body : "");

	res = curl_easy_perform(upload->curl);
	if (res != CURLE_OK) {
		counts->failed++;
		ffm_warn("HLS %s upload failed: %s (%s)", kind, curl_easy_strerror(res), req->url);
		return;
	}

	curl_easy_getinfo(upload->curl, CURLINFO_RESPONSE_CODE, &status);

	if (status >= 200 && status < 300) {
		counts->accepted++;
		return;
	}

	counts->refused++;
	ffm_warn("HLS %s refused with HTTP %ld: %s", kind, status, req->url);
}

/* ------------------------------------------------------------------------- */

static int hls_upload_io_open(AVFormatContext *s, AVIOContext **pb, const char *url, int flags, AVDictionary **options)
{
	struct hls_upload *upload = s->opaque;
	struct hls_upload_req *req;
	AVDictionaryEntry *method;
	unsigned char *buffer;

	/* hlsenc opens finished segments for reading when appending, and writes
	 * local files and crypto: URLs that carry no HTTP status to inspect. */
	if ((flags & AVIO_FLAG_READ_WRITE) != AVIO_FLAG_WRITE || !hls_upload_is_http(url)) {
		return upload->orig_io_open(s, pb, url, flags, options);
	}

	req = av_mallocz(sizeof(*req));
	if (!req) {
		return AVERROR(ENOMEM);
	}

	/* hlsenc expires old segments through this same callback, so the verb comes
	 * from the open rather than being assumed to be the upload one. */
	method = options ? av_dict_get(*options, "method", NULL, 0) : NULL;

	req->url = av_strdup(url);
	req->method = av_strdup(method && method->value ? method->value : "PUT");
	buffer = av_malloc(AVIO_BUFFER_SIZE);

	if (!req->url || !req->method || !buffer) {
		av_free(buffer);
		hls_upload_req_free(req);
		return AVERROR(ENOMEM);
	}

	*pb = avio_alloc_context(buffer, AVIO_BUFFER_SIZE, 1, req, NULL, hls_upload_write, NULL);
	if (!*pb) {
		av_free(buffer);
		hls_upload_req_free(req);
		return AVERROR(ENOMEM);
	}

	return 0;
}

static int hls_upload_io_close2(AVFormatContext *s, AVIOContext *pb)
{
	struct hls_upload *upload = s->opaque;
	struct hls_upload_req *req;

	if (!pb) {
		return 0;
	}

	if (pb->write_packet != hls_upload_write) {
		return upload->orig_io_close2(s, pb);
	}

	avio_flush(pb);
	req = pb->opaque;

	if (pb->error < 0) {
		/* avio latches the first write failure and drops every write after it,
		 * so the body assembled here is short. Sending it would hand the
		 * receiver a truncated segment it would answer 200 to, which reports as
		 * delivered and plays as corrupt. */
		const char *kind;
		struct hls_upload_counts *counts = hls_upload_bucket(upload, req->url, &kind);

		counts->truncated++;
		ffm_warn("HLS %s truncated locally, not sent: %s", kind, req->url);
	} else {
		hls_upload_perform(upload, req);
	}

	av_free(pb->buffer);
	avio_context_free(&pb);
	hls_upload_req_free(req);

	/* Nothing here is propagated: by the time hlsenc closes the upload it has
	 * already advanced the media sequence and written the segment into the
	 * playlist, so failing the close drops the stream without recovering the
	 * segment. */
	return 0;
}

/* ------------------------------------------------------------------------- */

bool hls_upload_install(AVFormatContext *output, AVDictionary *settings)
{
	struct hls_upload *upload;
	AVDictionaryEntry *entry;
	long timeout_ms = UPLOAD_TIMEOUT_DEFAULT_MS;

	if (!output->oformat || strcmp(output->oformat->name, "hls") != 0) {
		return false;
	}

	if (!output->url || !hls_upload_is_http(output->url)) {
		return false;
	}

	/* hlsenc in persistent mode drives the connection itself and requires every
	 * context it closes to be one of its own HTTP contexts, so a context opened
	 * here aborts the process rather than falling back. The single connection
	 * reused below is what replaces persistent mode. */
	entry = av_dict_get(settings, "http_persistent", NULL, 0);
	if (entry && entry->value && strcmp(entry->value, "0") != 0 && av_strcasecmp(entry->value, "false") != 0) {
		ffm_warn("HLS upload status is unavailable: http_persistent is set");
		return false;
	}

	entry = av_dict_get(settings, "timeout", NULL, 0);
	if (entry && entry->value) {
		int64_t timeout_us = 0;

		/* Every form of this value goes through the parser hlsenc applies to the
		 * same option, so the socket deadline and this one cannot read one
		 * string two ways. curl reads a zero timeout as "wait forever", so a
		 * value that rounds away is refused rather than passed on. */
		if (av_parse_time(&timeout_us, entry->value, 1) < 0 || timeout_us < 1000) {
			ffm_warn("HLS upload cannot use timeout '%s', keeping %ld ms", entry->value, timeout_ms);
		} else {
			timeout_ms = (long)(timeout_us / 1000);
		}
	}

	upload = av_mallocz(sizeof(*upload));
	if (!upload) {
		return false;
	}

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		ffm_warn("HLS upload status is unavailable: curl could not be initialized");
		av_free(upload);
		return false;
	}

	upload->curl = curl_easy_init();
	if (!upload->curl) {
		ffm_warn("HLS upload status is unavailable: no curl handle");
		curl_global_cleanup();
		av_free(upload);
		return false;
	}

	/* Keep the request on the wire the same shape the FFmpeg HTTP writer would
	 * have sent, so taking the upload over cannot by itself change how an ingest
	 * answers: no Expect/100-continue round trip, no curl-supplied form content
	 * type, and HTTP/1.1 rather than whatever curl would negotiate. */
	upload->headers = curl_slist_append(NULL, "Expect:");
	upload->headers = curl_slist_append(upload->headers, "Content-Type:");
	curl_easy_setopt(upload->curl, CURLOPT_HTTPHEADER, upload->headers);
	curl_easy_setopt(upload->curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
	curl_easy_setopt(upload->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(upload->curl, CURLOPT_POSTREDIR, (long)CURL_REDIR_POST_ALL);

	curl_easy_setopt(upload->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(upload->curl, CURLOPT_WRITEFUNCTION, hls_upload_discard);
	curl_easy_setopt(upload->curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(upload->curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);

	entry = av_dict_get(settings, "http_user_agent", NULL, 0);
	if (entry && entry->value) {
		curl_easy_setopt(upload->curl, CURLOPT_USERAGENT, entry->value);
	}

	upload->orig_io_open = output->io_open;
	upload->orig_io_close2 = output->io_close2;

	output->opaque = upload;
	output->io_open = hls_upload_io_open;
	output->io_close2 = hls_upload_io_close2;

	return true;
}

struct hls_upload *hls_upload_state(const AVFormatContext *output)
{
	if (!output || output->io_open != hls_upload_io_open) {
		return NULL;
	}

	return output->opaque;
}

void hls_upload_finish(struct hls_upload *upload)
{
	if (!upload) {
		return;
	}

	/* Only losses are reported. The parent tags everything on this pipe as a
	 * warning, so an all-clear line would read as a problem in the user's log. */
	if (hls_upload_counts_lost(&upload->segment) || hls_upload_counts_lost(&upload->playlist)) {
		ffm_warn("HLS uploads: segments %u accepted, %u refused, %u failed, %u truncated; "
			 "playlists %u accepted, %u refused, %u failed, %u truncated",
			 upload->segment.accepted, upload->segment.refused, upload->segment.failed,
			 upload->segment.truncated, upload->playlist.accepted, upload->playlist.refused,
			 upload->playlist.failed, upload->playlist.truncated);
	}

	curl_slist_free_all(upload->headers);
	curl_easy_cleanup(upload->curl);
	curl_global_cleanup();
	av_free(upload);
}
