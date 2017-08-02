/*
 * Copyright 2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * dup.c - a dummy prog using pmemfile, using dup, dup2 via libc
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "compiler_utils.h"

static int
xcreate(const char *path)
{
	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0700);
	if (fd < 0)
		err(1, "open(\"%s\")", path);

	return fd;
}

static void
xclose(int fd)
{
	if (close(fd) != 0)
		err(1, "close");
}

static int
xdup(int fd)
{
	int new_fd = dup(fd);
	if (new_fd < 0)
		err(1, "dup in");

	return new_fd;
}

static off_t
xlseek(int fd, off_t offset, int whence)
{
	off_t r = lseek(fd, offset, whence);
	if (r == -1)
		err(1, "lseek(%d, %ji, %d)", fd, offset, whence);

	return r;
}

static void
xwrite(int fd, const void *buffer, size_t size)
{
	if (write(fd, buffer, size) != (ssize_t)size)
		err(1, "write");
}

static void
xread(int fd, void *buffer, size_t size)
{
	if (read(fd, buffer, size) != (ssize_t)size)
		err(1, "write");
}

static void
seek_and_destroy(int fd0, int fd1)
{
	off_t offset = 0;
	assert(xlseek(fd0, offset, SEEK_SET) == offset);
	assert(xlseek(fd1, offset, SEEK_CUR) == offset);

	offset = 0x10;
	assert(xlseek(fd0, offset, SEEK_SET) == offset);
	assert(xlseek(fd1, 0, SEEK_CUR) == offset);

	offset = 0x40;
	assert(xlseek(fd1, offset, SEEK_SET) == offset);
	assert(xlseek(fd0, 0, SEEK_CUR) == offset);

	const char buffer0[] = "My hovercraft is full of eels!";
	char buffer1[sizeof(buffer0)];

	xwrite(fd0, buffer0, sizeof(buffer0));

	offset += (off_t)sizeof(buffer0);

	assert(xlseek(fd0, 0, SEEK_CUR) == offset);
	assert(xlseek(fd1, 0, SEEK_CUR) == offset);

	offset -= (off_t)sizeof(buffer0);
	assert(xlseek(fd1, -((off_t)sizeof(buffer0)), SEEK_CUR) == offset);
	assert(xlseek(fd0, 0, SEEK_CUR) == offset);

	xread(fd1, buffer1, sizeof(buffer1));
	assert(memcmp(buffer0, buffer1, sizeof(buffer0)) == 0);

	offset += (off_t)sizeof(buffer0);

	assert(xlseek(fd0, 0, SEEK_CUR) == offset);
	assert(xlseek(fd1, 0, SEEK_CUR) == offset);

	xclose(fd0);
	xclose(fd1);
}

static void
test(const char *path)
{
	int fd[0x40];

	fputs("fd and dup'ed fd\n", stderr);
	fd[0] = xcreate(path);
	fd[1] = xdup(fd[0]);
	seek_and_destroy(fd[0], fd[1]);

	fputs("dup'ed fd and original ed fd\n", stderr);
	fd[0] = xcreate(path);
	fd[1] = xdup(fd[0]);
	seek_and_destroy(fd[1], fd[0]);

	fputs("fd array\n", stderr);

	fd[0] = xcreate(path);
	for (size_t i = 1; i < ARRAY_SIZE(fd); ++i)
		fd[i] = xdup(fd[i - 1]);

	size_t left = 0;
	size_t right = ARRAY_SIZE(fd) - 1;

	while (left < right)
		seek_and_destroy(fd[left++], fd[right--]);

	fputs("post-close checking\n", stderr);
	for (size_t i = 1; i < ARRAY_SIZE(fd); ++i) {
		errno = 0;
		assert(lseek(fd[i], 1, SEEK_SET) == -1);
		assert(errno == EBADF);
	}
}

int
main(int argc, char **argv)
{
	if (argc < 3)
		errx(1, "two path arguments required");

	(void) xdup(2);

	if (dup(77) >= 0)
		errx(1, "dup of non exiting fd did not fail");

	char path_in_kernel[strlen(argv[2]) + 0x10];
	char path_in_pmemf[strlen(argv[1]) + 0x10];

	strcpy(path_in_kernel, argv[1]);
	strcat(path_in_kernel, "filename");
	strcpy(path_in_pmemf, argv[2]);
	strcat(path_in_pmemf, "filename");

	fputs("Testing with kernel handled files\n", stderr);
	test(path_in_kernel);

	fputs("Testing with pmemfile handled files\n", stderr);
	test(path_in_pmemf);

	return 0;
}
