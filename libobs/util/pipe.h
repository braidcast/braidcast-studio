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

#include "c99defs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct os_process_pipe;
typedef struct os_process_pipe os_process_pipe_t;

struct os_process_args;
typedef struct os_process_args os_process_args_t;

EXPORT os_process_pipe_t *os_process_pipe_create(const char *cmd_line, const char *type);
EXPORT os_process_pipe_t *os_process_pipe_create2(const os_process_args_t *args, const char *type);
/* Closes the read end of the child's stderr before waiting for it to exit, so a
 * child that writes past the pipe buffer fails its write rather than blocking in
 * it while this waits for the exit that write is holding up. Use the draining
 * form below to read what the child writes on its way out. */
EXPORT int os_process_pipe_destroy(os_process_pipe_t *pp);

/* Destroys the pipe as above, but keeps the child's stderr readable until it has
 * exited. A child only learns the work is over when its stdin closes, so
 * whatever it does last -- for a muxer, writing the trailer -- produces its
 * errors after the point where the plain form has already dropped the read end.
 *
 * `drain` is called while waiting and once more after the child exits. It is
 * also what keeps the child from blocking in write() against a stderr pipe
 * nobody is emptying, so it must actually consume what is there. */
EXPORT int os_process_pipe_destroy_drain(os_process_pipe_t *pp, void (*drain)(void *param), void *param);

/* How often that drain runs while the child is still alive. The stderr pipe
 * holds a few kilobytes, so emptying it this often is what makes it impossible
 * for the child to block in write() and never reach the exit being waited on. */
#define OS_PROCESS_PIPE_DRAIN_POLL_MS 50

EXPORT size_t os_process_pipe_read(os_process_pipe_t *pp, uint8_t *data, size_t len);
EXPORT size_t os_process_pipe_read_err(os_process_pipe_t *pp, uint8_t *data, size_t len);

/* Reads only what the child has already written, and returns 0 rather than
 * waiting for it to write more. This is what makes it safe to drain a child's
 * stderr on a timer: the blocking form above can only be called once the child
 * is known to have something to say, which in practice means once it has died,
 * and a child whose stderr nobody reads blocks in write() as soon as the pipe
 * buffer fills.
 *
 * Do not interleave with os_process_pipe_read_err on the same pipe. That one
 * reads through the platform's buffered stdio and this one does not, so bytes
 * consumed here can leave the blocking call waiting for data that has already
 * been delivered. Pick one per pipe. */
EXPORT size_t os_process_pipe_read_err_avail(os_process_pipe_t *pp, uint8_t *data, size_t len);
EXPORT size_t os_process_pipe_write(os_process_pipe_t *pp, const uint8_t *data, size_t len);

EXPORT struct os_process_args *os_process_args_create(const char *executable);
EXPORT void os_process_args_add_arg(struct os_process_args *args, const char *arg);
#ifndef _MSC_VER
__attribute__((__format__(__printf__, 2, 3)))
#endif
EXPORT void
os_process_args_add_argf(struct os_process_args *args, const char *format, ...);
EXPORT char **os_process_args_get_argv(const struct os_process_args *args);
EXPORT size_t os_process_args_get_argc(struct os_process_args *args);
EXPORT void os_process_args_destroy(struct os_process_args *args);

#ifdef __cplusplus
}
#endif
