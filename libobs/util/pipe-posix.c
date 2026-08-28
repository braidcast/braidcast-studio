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

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <spawn.h>
#include <fcntl.h>
#include <poll.h>

#include "bmem.h"
#include "pipe.h"
#include "platform.h"

extern char **environ;

struct os_process_pipe {
	bool read_pipe;
	int pid;
	FILE *file;
	FILE *err_file;
};

os_process_pipe_t *os_process_pipe_create_internal(const char *bin, char **argv, const char *type)
{
	struct os_process_pipe process_pipe = {0};
	struct os_process_pipe *out;
	posix_spawn_file_actions_t file_actions;

	if (!bin || !argv || !type) {
		return NULL;
	}

	process_pipe.read_pipe = *type == 'r';

	int mainfds[2] = {0};
	int errfds[2] = {0};

	if (pipe(mainfds) != 0) {
		return NULL;
	}

	if (pipe(errfds) != 0) {
		close(mainfds[0]);
		close(mainfds[1]);

		return NULL;
	}

	if (posix_spawn_file_actions_init(&file_actions) != 0) {
		close(mainfds[0]);
		close(mainfds[1]);
		close(errfds[0]);
		close(errfds[1]);

		return NULL;
	}

	fcntl(mainfds[0], F_SETFD, FD_CLOEXEC);
	fcntl(mainfds[1], F_SETFD, FD_CLOEXEC);
	fcntl(errfds[0], F_SETFD, FD_CLOEXEC);
	fcntl(errfds[1], F_SETFD, FD_CLOEXEC);

	if (process_pipe.read_pipe) {
		posix_spawn_file_actions_addclose(&file_actions, mainfds[0]);
		if (mainfds[1] != STDOUT_FILENO) {
			posix_spawn_file_actions_adddup2(&file_actions, mainfds[1], STDOUT_FILENO);
			posix_spawn_file_actions_addclose(&file_actions, mainfds[1]);
		}
	} else {
		posix_spawn_file_actions_addclose(&file_actions, mainfds[1]);
		if (mainfds[0] != STDIN_FILENO) {
			posix_spawn_file_actions_adddup2(&file_actions, mainfds[0], STDIN_FILENO);
			posix_spawn_file_actions_addclose(&file_actions, mainfds[0]);
		}
	}

	posix_spawn_file_actions_addclose(&file_actions, errfds[0]);
	if (errfds[1] != STDERR_FILENO) {
		posix_spawn_file_actions_adddup2(&file_actions, errfds[1], STDERR_FILENO);
		posix_spawn_file_actions_addclose(&file_actions, errfds[1]);
	}

	int pid;
	int ret = posix_spawn(&pid, bin, &file_actions, NULL, (char *const *)argv, environ);

	posix_spawn_file_actions_destroy(&file_actions);

	if (ret != 0) {
		close(mainfds[0]);
		close(mainfds[1]);
		close(errfds[0]);
		close(errfds[1]);

		return NULL;
	}

	close(errfds[1]);
	process_pipe.err_file = fdopen(errfds[0], "r");

	if (process_pipe.read_pipe) {
		close(mainfds[1]);
		process_pipe.file = fdopen(mainfds[0], "r");
	} else {
		close(mainfds[0]);
		process_pipe.file = fdopen(mainfds[1], "w");
	}

	process_pipe.pid = pid;

	out = bmalloc(sizeof(os_process_pipe_t));
	*out = process_pipe;
	return out;
}

os_process_pipe_t *os_process_pipe_create(const char *cmd_line, const char *type)
{
	if (!cmd_line) {
		return NULL;
	}

	char *argv[4] = {"sh", "-c", (char *)cmd_line, NULL};
	return os_process_pipe_create_internal("/bin/sh", argv, type);
}

os_process_pipe_t *os_process_pipe_create2(const os_process_args_t *args, const char *type)
{
	char **argv = os_process_args_get_argv(args);
	return os_process_pipe_create_internal(argv[0], argv, type);
}

int os_process_pipe_destroy_drain(os_process_pipe_t *pp, void (*drain)(void *param), void *param)
{
	int ret = 0;

	if (pp) {
		int status = 0;

		fclose(pp->file);
		pp->file = NULL;

		if (!drain) {
			fclose(pp->err_file);
			pp->err_file = NULL;
		}

		do {
			ret = waitpid(pp->pid, &status, drain ? WNOHANG : 0);
			if (drain && ret == 0) {
				drain(param);
				os_sleep_ms(OS_PROCESS_PIPE_DRAIN_POLL_MS);
			}
		} while (ret == 0 || (ret == -1 && errno == EINTR));

		if (drain) {
			drain(param);
			fclose(pp->err_file);
			pp->err_file = NULL;
		}

		/* Only a reap fills in `status`; on a failed wait `ret` carries the
		 * errno-bearing -1 instead. */
		if (ret > 0 && WIFEXITED(status)) {
			ret = (int)(char)WEXITSTATUS(status);
		}
		bfree(pp);
	}

	return ret;
}

int os_process_pipe_destroy(os_process_pipe_t *pp)
{
	return os_process_pipe_destroy_drain(pp, NULL, NULL);
}

size_t os_process_pipe_read(os_process_pipe_t *pp, uint8_t *data, size_t len)
{
	if (!pp) {
		return 0;
	}
	if (!pp->read_pipe) {
		return 0;
	}

	return fread(data, 1, len, pp->file);
}

size_t os_process_pipe_read_err(os_process_pipe_t *pp, uint8_t *data, size_t len)
{
	if (!pp) {
		return 0;
	}

	return fread(data, 1, len, pp->err_file);
}

size_t os_process_pipe_read_err_avail(os_process_pipe_t *pp, uint8_t *data, size_t len)
{
	if (!pp || !pp->err_file) {
		return 0;
	}

	/* Deliberately the raw descriptor rather than err_file: a poll that says
	 * readable promises read() will not block, and promises nothing about how
	 * much fread would sit waiting for. See the header on not mixing the two. */
	const int fd = fileno(pp->err_file);
	struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};

	if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
		return 0;
	}

	const ssize_t ret = read(fd, data, len);
	return ret > 0 ? (size_t)ret : 0;
}

size_t os_process_pipe_write(os_process_pipe_t *pp, const uint8_t *data, size_t len)
{
	if (!pp) {
		return 0;
	}
	if (pp->read_pipe) {
		return 0;
	}

	size_t written = 0;
	while (written < len) {
		size_t ret = fwrite(data + written, 1, len - written, pp->file);
		if (!ret) {
			return written;
		}

		written += ret;
	}
	return written;
}
