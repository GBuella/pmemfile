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

#include "lock_free_iterator.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "utils.h"

/*
 * is_offset_in_block - checks if block pointer is not null, and the offset
 * is in the range described by the pmemfile_block_desc.
 */
static bool
is_offset_in_block(size_t offset, const struct pmemfile_block_desc *block)
{
	if (block == NULL)
		return false;

	return block->offset <= offset && offset < block->offset + block->size;
}

/*
 * find_first_initialized_block_after - looks for the first block (in ascending
 * order according to block->offset) following the argument block, that refers
 * to initialized data.
 *
 * The block pointer argument is expected to be the result of
 * iterate_on_file_range.
 *
 * If the block argument is NULL, looks for the first initialized block
 * in the file - returns NULL if none found.
 *
 * If the block argument is not NULL, it looks for the first initialized
 * block following the said block -- returns NULL if none found, never
 * returns the same block pointer.
 */
static const struct pmemfile_block_desc *
find_first_initialized_block_after(PMEMfilepool *pfp,
					const struct pmemfile_vinode *vinode,
					const struct pmemfile_block_desc *block)
{
	if (block == NULL)
		block = vinode->first_block;

	while (block != NULL && ((block->flags & BLOCK_INITIALIZED) == 0))
		block = PF_RO(pfp, block->next);

	return block;
}

static void
lfit_setup_range(PMEMfilepool *pfp, struct lock_free_iterator *lfit,
		const struct pmemfile_vinode *vinode,
		const struct pmemfile_block_desc *block,
		size_t offset, size_t file_size)
{
	if ((!is_offset_in_block(offset, block)) ||
	    ((block->flags & BLOCK_INITIALIZED) == 0)) {
		lfit->address = NULL;
		/*
		 * The offset does not point into an initialized block, the
		 * fastpath routine should read zeros - up until the start of
		 * the next initialized block, or until EOF.
		 */
		block = find_first_initialized_block_after(pfp, vinode, block);
		if (block != NULL && block->offset < file_size)
			lfit->length = block->offset - offset;
		else
			lfit->length = file_size - offset;

	} else {
		/*
		 * The offset does point into an initialized block, the
		 * fastpath routine should read data from it until the end
		 * of the block.
		 */
		size_t offset_in_block = offset - block->offset;

		lfit->address = PF_RO(pfp, block->data) + offset_in_block;
		lfit->length = block->size - offset_in_block;
	}
}

/*
 * lfit_setup -- setup a lock_free_iterator to point to data in a file
 * corresponding to an offset.
 *
 * The file, and the vinode must be locked while calling lfit_setup.
 * But while accessing the *lfit in the read fast path, only the file
 * needs to be locked, as the vinode is not accessed there.
 *
 * The block is expected to point to be a return value of iterate_on_file_range
 * after performing a read or write, while the offset is expected to be
 * the new file offset after the said read/write.
 *
 * The next time lfit is accessed, it is going to point to data at that offset.
 * If there is no data in the file at that offset, lfit just caches the count
 * of bytes till the next data in the file (first initialized byte in the file
 * at file->offset + lfit->length).
 */
void
lfit_setup(PMEMfilepool *pfp, struct lock_free_iterator *lfit,
		const struct pmemfile_vinode *vinode,
		struct pmemfile_block_desc *block,
		size_t offset, size_t file_size)
{
	if (lfit == NULL)
		return;

	if (offset >= file_size) { /* EOF */
		lfit_invalidate(lfit);
		return;
	}

	lfit->last_pre_write_counter = vinode->pre_write_counter;
	lfit->last_post_write_counter = vinode->post_write_counter;

	lfit->block_pointer_cache = block;

	lfit_setup_range(pfp, lfit, vinode, block, offset, file_size);
}

/*
 * is_modification_indicated -- checks if the counters match.
 * If any of the two counters differ from the ones read from vinode, that
 * means some content or metadata of the file was modified since the counters
 * were set up in lfit_setup.
 */
static bool
is_modification_indicated(const struct lock_free_iterator *it,
				const struct pmemfile_vinode *vinode)
{
	return (it->last_pre_write_counter != vinode->pre_write_counter) ||
		(it->last_post_write_counter != vinode->post_write_counter);
}

/*
 * try_read_fastpath -- attempt to perform a read operation, without holding
 * the read-write-lock associated with the vinode instance.
 *
 * The file lock_free_iterator itself can still only be accessed under mutual
 * exclusion, thus the PMEMfile containing this iterator should be locked.
 */
pmemfile_ssize_t
try_read_fastpath(struct lock_free_iterator *it, struct pmemfile_vinode *vinode,
			void *buffer, size_t count)
{
	if (count == 0)
		return 0;

	if (!is_lfit_initialized(it))
		return -1;

	if (is_modification_indicated(it, vinode))
		return -1;

	if (count > it->length)
		return -1;

	if (lfit_reads_as_zero(it)) {
		memset(buffer, 0, count);
	} else if (count <= READ_FAST_PATH_TRESHOLD) {
		char local_copy[count];

		memcpy(local_copy, it->address, count);

		if (is_modification_indicated(it, vinode))
			return -1;

		memcpy(buffer, local_copy, count);

		it->address += count;
	} else {
		return -1;
	}

	it->length -= count;
	return (pmemfile_ssize_t) count;
}
