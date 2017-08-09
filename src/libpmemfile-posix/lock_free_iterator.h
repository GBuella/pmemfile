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

#ifndef PMEMFILE_LOCK_FREE_ITERATOR_H
#define PMEMFILE_LOCK_FREE_ITERATOR_H

#include <stdbool.h>
#include <stddef.h>

#include "inode.h"


/*
 * READ_FAST_PATH_TRESHOLD - in pmemfile_read, and pmemfile_readv calls
 * with length lower than or equal to this treshold, pmemfile attempts to
 * perform the operation without locking the vinode.
 *
 * For such reads, a temporary buffer is allocated on the stack - so setting
 * this treshold to a million results in pmemfile attempting to allocate a
 * megabyte on the stack.
 * Also, during such lockless reads, the data is copied twice (once from pmem
 * to the temporary buffer, then from the temporary buffer to the client
 * buffer).
 *
 * Therefore this treshold should be a reasonably low number.
 */
#define READ_FAST_PATH_TRESHOLD 256

struct lock_free_iterator {
	struct pmemfile_block_desc *block_pointer_cache;
	uint64_t last_pre_write_counter;
	uint64_t last_post_write_counter;
	const char *address;
	size_t length;
};

static inline bool
is_lfit_initialized(const struct lock_free_iterator *lfit)
{
	/*
	 * The address field can be NULL, or a valid pointer,
	 * using the length field to indicate wether the struct
	 * is usable or not.
	 */
	return lfit != NULL && lfit->length > 0;
}

static inline void
lfit_invalidate(struct lock_free_iterator *lfit)
{
	lfit->length = 0;
}

static inline bool
lfit_reads_as_zero(struct lock_free_iterator *lfit)
{
	return lfit->address == NULL;
}

void lfit_setup(PMEMfilepool *, struct lock_free_iterator *,
		const struct pmemfile_vinode *,
		struct pmemfile_block_desc *,
		size_t offset, size_t file_size);

pmemfile_ssize_t try_read_fastpath(struct lock_free_iterator *,
			struct pmemfile_vinode *, void *buffer, size_t count);
#endif
