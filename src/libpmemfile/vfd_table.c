/*
 * Copyright 2016-2017, Intel Corporation
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

#include "vfd_table.h"

#include <assert.h>
#include <stdbool.h>
#include <fcntl.h>
#include <syscall.h>

#include <libsyscall_intercept_hook_point.h>
#include <libpmemfile-posix.h>

#include "sys_util.h"
#include "preload.h"

struct vfile_description {
	struct pool_description *pool;
	PMEMfile *file;
	long kernel_cwd_fd;
	bool is_special_cwd_desc;
	int ref_count;
};

static void
vf_ref_count_inc(struct vfile_description *entry)
{
	__atomic_add_fetch(&entry->ref_count, 1, __ATOMIC_ACQ_REL);
}

static int
vf_ref_count_dec_and_fetch(struct vfile_description *entry)
{
	return __atomic_sub_fetch(&entry->ref_count, 1, __ATOMIC_ACQ_REL);
}

static struct vfile_description *cwd_entry;
static struct vfile_description *vfd_table[0x8000];

static pthread_mutex_t vfd_table_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct vfile_description *free_vfile_slots[2 * ARRAY_SIZE(vfd_table)];
static pthread_mutex_t free_vfile_slot_mutex = PTHREAD_MUTEX_INITIALIZER;

static void
mark_as_free_file_slot(struct vfile_description *entry)
{
	static unsigned insert_index;

	util_mutex_lock(&free_vfile_slot_mutex);

	assert(entry->ref_count == 0);

	free_vfile_slots[insert_index] = entry;
	++insert_index;
	insert_index %= ARRAY_SIZE(free_vfile_slots);

	util_mutex_unlock(&free_vfile_slot_mutex);
}

static struct vfile_description *
fetch_free_file_slot(void)
{
	static unsigned fetch_index;

	struct vfile_description *entry;

	util_mutex_lock(&free_vfile_slot_mutex);

	entry = free_vfile_slots[fetch_index];
	++fetch_index;
	fetch_index %= ARRAY_SIZE(free_vfile_slots);

	util_mutex_unlock(&free_vfile_slot_mutex);

	return entry;
}

static void
setup_free_slots(void)
{
	static struct vfile_description store[ARRAY_SIZE(free_vfile_slots) - 1];

	for (unsigned i = 0; i < ARRAY_SIZE(store); ++i)
		mark_as_free_file_slot(store + i);
}

static struct vfd_reference
pmemfile_ref_vfd_under_mutex(long vfd)
{
	struct vfile_description *entry = vfd_table[vfd];

	if (entry == NULL)
		return (struct vfd_reference) {.kernel_fd = vfd, };

	vf_ref_count_inc(entry);

	return (struct vfd_reference) {
	    .pool = entry->pool, .file = entry->file, .internal = entry, };
}

static bool
is_in_vfd_table_range(long number)
{
	return (number >= 0) && (number < (long)ARRAY_SIZE(vfd_table));
}

static bool
can_be_in_vfd_table(long vfd)
{
	if (!is_in_vfd_table_range(vfd))
		return false;

	return __atomic_load_n(vfd_table + vfd, __ATOMIC_CONSUME) != NULL;
}

struct vfd_reference
pmemfile_vfd_ref(long vfd)
{
	if (!can_be_in_vfd_table(vfd))
		return (struct vfd_reference) {.kernel_fd = vfd, };

	util_mutex_lock(&vfd_table_mutex);

	struct vfd_reference result = pmemfile_ref_vfd_under_mutex(vfd);

	util_mutex_unlock(&vfd_table_mutex);

	return result;
}

static struct vfd_reference
get_fdcwd_reference(void)
{
	struct vfd_reference result;

	util_mutex_lock(&vfd_table_mutex);

	vf_ref_count_inc(cwd_entry);

	result.internal = cwd_entry;

	result.kernel_fd = cwd_entry->kernel_cwd_fd;
	result.pool = cwd_entry->pool;
	result.file = cwd_entry->file;

	util_mutex_unlock(&vfd_table_mutex);

	return result;
}

struct vfd_reference
pmemfile_vfd_at_ref(long vfd)
{
	if (vfd == AT_FDCWD)
		return get_fdcwd_reference();
	else
		return pmemfile_vfd_ref(vfd);
}

static void
unref_entry(struct vfile_description *entry)
{
	if (entry == NULL)
		return;

	if (vf_ref_count_dec_and_fetch(entry) == 0) {
		if (entry->is_special_cwd_desc)
			syscall_no_intercept(SYS_close, entry->kernel_cwd_fd);
		else
			pmemfile_close(entry->pool->pool, entry->file);
		mark_as_free_file_slot(entry);
	}
}

void
pmemfile_vfd_unref(struct vfd_reference ref)
{
	unref_entry(ref.internal);
}

long
pmemfile_vfd_dup(long vfd)
{
	long result;

	if (can_be_in_vfd_table(vfd)) {
		util_mutex_lock(&vfd_table_mutex);

		result = syscall_no_intercept(SYS_dup, vfd);

		if (result >= 0 && vfd_table[vfd] != NULL) {
			assert(vfd_table[result] == NULL);

			vf_ref_count_inc(vfd_table[vfd]);
			__atomic_store_n(vfd_table + result,
			    vfd_table[vfd], __ATOMIC_RELEASE);
		}

		util_mutex_unlock(&vfd_table_mutex);
	} else {
		result = syscall_no_intercept(SYS_dup, vfd);
	}

	return result;
}

long
pmemfile_vfd_dup2(long old_vfd, long new_vfd)
{
	long result;

	if (can_be_in_vfd_table(old_vfd)) {
		util_mutex_lock(&vfd_table_mutex);

		result = syscall_no_intercept(SYS_dup2, old_vfd, new_vfd);

		if (result >= 0 && vfd_table[old_vfd] != NULL) {
			assert(result == new_vfd);
			assert(vfd_table[new_vfd] == NULL);

			vf_ref_count_inc(vfd_table[old_vfd]);
			__atomic_store_n(vfd_table + new_vfd,
			    vfd_table[old_vfd], __ATOMIC_RELEASE);
		}

		util_mutex_unlock(&vfd_table_mutex);
	} else {
		result = syscall_no_intercept(SYS_dup2, old_vfd, new_vfd);
	}

	return result;
}

long
pmemfile_vfd_close(long vfd)
{
	struct vfile_description *entry = NULL;

	if (can_be_in_vfd_table(vfd)) {
		util_mutex_lock(&vfd_table_mutex);

		entry = vfd_table[vfd];
		vfd_table[vfd] = NULL;

		util_mutex_unlock(&vfd_table_mutex);
	}

	long result = syscall_no_intercept(SYS_close, vfd);

	if (entry != NULL) {
		unref_entry(entry);
		result = 0;
	}

	return result;
}

static void
setup_cwd(void)
{
	long fd = syscall_no_intercept(SYS_open, ".", O_DIRECTORY | O_RDONLY);
	if (fd < 0)
		exit_with_msg(1, "setup_cwd");

	cwd_entry = fetch_free_file_slot();
	*cwd_entry = (struct vfile_description) {
		.pool = NULL, .file = NULL,
		.kernel_cwd_fd = fd,
		.is_special_cwd_desc = true,
		.ref_count = 1};
}

static void
chdir_exchange_entry(struct vfile_description *new_cwd_entry)
{
	struct vfile_description *old_cwd_entry;

	/*
	 * Overwrite the original cwd entry with the new one. This looks
	 * like it could be done using __atomic_exchange_n, since all that
	 * happens under the mutex is the exchange of a single integer. But
	 * that could race with pmemfile_vfd_ref. In the example below, the
	 * vf_ref_count_inc in step #4 refers to an entry that is already
	 * deallocated in step #3 (and possibly allocated again for some
	 * other file).
	 *
	 *   |T0:                        | T1:                           |
	 *   |pmemfile_vfd_ref call      | chdir_exchange_entry call     |
	 *   |                           |                               |
	 * 0:|lock(vfd_table_mutex)      |                               |
	 * 1:|entry = vfd_table[vfd];    |                               |
	 * 2:|                           | exchange(&cwd_entry, entry);  |
	 * 3:|                           | unref(old_cwd_entry);         |
	 * 4:|vf_ref_count_inc(entry);   |                               |
	 * 5:|unlock(vfd_table_mutex);   |                               |
	 *
	 */
	util_mutex_lock(&vfd_table_mutex);

	old_cwd_entry = cwd_entry;
	cwd_entry = new_cwd_entry;

	util_mutex_unlock(&vfd_table_mutex);

	unref_entry(old_cwd_entry);
}

long
pmemfile_vfd_chdir_pf(struct pool_description *pool, struct pmemfile_file *file)
{
	struct vfile_description *entry = fetch_free_file_slot();

	*entry = (struct vfile_description) {
		.pool = pool, .file = file,
		.kernel_cwd_fd = -1,
		.is_special_cwd_desc = false,
		.ref_count = 1};

	chdir_exchange_entry(entry);

	return 0;
}

long
pmemfile_vfd_chdir_kernel_fd(long fd)
{
	long result = syscall_no_intercept(SYS_fchdir, fd);

	if (result < 0)
		return result;

	struct vfile_description *entry = fetch_free_file_slot();

	*entry = (struct vfile_description) {
		.pool = NULL, .file = NULL,
		.kernel_cwd_fd = fd,
		.is_special_cwd_desc = true,
		.ref_count = 1};

	chdir_exchange_entry(entry);

	return result;
}

static bool is_memfd_syscall_available;

#ifdef SYS_memfd_create

static void
check_memfd_syscall(void)
{
	long fd = syscall_no_intercept(SYS_memfd_create, "check", 0);
	if (fd >= 0) {
		is_memfd_syscall_available = true;
		syscall_no_intercept(SYS_close, fd);
	}
}

#else

#define SYS_memfd_create 0
#define check_memfd_syscall()

#endif

/*
 * acquire_new_fd - grab a new file descriptor from the kernel
 */
static long
acquire_new_fd(const char *path)
{
	long fd;

	if (is_memfd_syscall_available) {
		fd = syscall_no_intercept(SYS_memfd_create, path, 0);
		/* memfd_create can fail for too long name */
		if (fd < 0) {
			fd = syscall_no_intercept(SYS_open, "/dev/null",
					O_RDONLY);
		}
	} else {
		fd = syscall_no_intercept(SYS_open, "/dev/null", O_RDONLY);
	}

	if (fd >= (long)ARRAY_SIZE(vfd_table)) {
		syscall_no_intercept(SYS_close, fd);
		return -ENFILE;
	}

	return fd;
}

long
pmemfile_vfd_assign(struct pool_description *pool,
			struct pmemfile_file *file,
			const char *path)
{
	long new_vfd = acquire_new_fd(path);

	if (new_vfd < 0)
		return new_vfd;

	struct vfile_description *entry = fetch_free_file_slot();

	*entry = (struct vfile_description) {
		.pool = pool, .file = file,
		.kernel_cwd_fd = -1,
		.is_special_cwd_desc = false,
		.ref_count = 1};

	util_mutex_lock(&vfd_table_mutex);

	vfd_table[new_vfd] = entry;

	util_mutex_unlock(&vfd_table_mutex);

	return new_vfd;
}

long
pmemfile_vfd_fchdir(long vfd)
{
	long result;
	struct vfile_description *old_cwd_entry = NULL;

	util_mutex_lock(&vfd_table_mutex);

	if (is_in_vfd_table_range(vfd) && vfd_table[vfd] != NULL) {
		vf_ref_count_inc(vfd_table[vfd]);
		old_cwd_entry = cwd_entry;
		cwd_entry = vfd_table[vfd];
		result = 0;
	} else {
		long new_fd = syscall_no_intercept(SYS_dup, vfd);
		if (new_fd >= 0)
			result = syscall_no_intercept(SYS_fchdir, new_fd);
		else
			result = new_fd;

		if (result == 0) {
			struct vfile_description *entry = fetch_free_file_slot();

			*entry = (struct vfile_description) {
				.pool = NULL, .file = NULL,
				.kernel_cwd_fd = new_fd,
				.is_special_cwd_desc = true,
				.ref_count = 1};

			old_cwd_entry = cwd_entry;
			cwd_entry = entry;
			result = 0;
		}
	}

	util_mutex_unlock(&vfd_table_mutex);

	if (old_cwd_entry != NULL)
		unref_entry(old_cwd_entry);

	return result;
}

void
pmemfile_vfd_table_init(void)
{
	check_memfd_syscall();
	setup_free_slots();
	setup_cwd();
}
