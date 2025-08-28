/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Pau Sum <pau@freebsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/endian.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <fs/ext2fs/ext2_dinode.h>
#include <fs/ext2fs/ext2_journal.h>
#include <fs/ext2fs/ext2_journal_debug.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>

#include <fs/ext2fs/ext2_extern.h>

MALLOC_DEFINE(M_EXT2JOURNAL, "ext2fs_journal", "In-memory ext2 journal");
MALLOC_DEFINE(M_EXT2JSB, "ext2fs_journal_sb", "In-memory copy of \
	journal superblock");
MALLOC_DEFINE(M_EXT2JTRANS, "ext2fs_journal_trans", "ext2 journal transaction");
MALLOC_DEFINE(M_EXT2JBUF, "ext2fs_journal_buf", "ext2 journal buffer descriptor");
MALLOC_DEFINE(M_EXT2REVOKE, "ext2fs_revoke", "ext2 journal revoke records");

// #define ENABLE_CHECKPOINT_WRITE

// #define DISABLE_RECOVERY

static struct ext2fs_journal_revoke_table *ext2_journal_revoke_table_create(
    void);
static void ext2_journal_revoke_table_destroy(
    struct ext2fs_journal_revoke_table *table);
static void ext2_journal_revoke_table_clear(
    struct ext2fs_journal_revoke_table *table);
static void ext2_journal_revoke_list_clear(
    struct ext2fs_journal_revoke_list *list);
static void ext2_journal_cancel_revoke(struct ext2fs_journal_transaction *trans,
    struct ext2fs_journal_buf *jbuf);
static int ext2_journal_write_revoke_block(struct ext2fs_journal *jrnp,
    struct ext2fs_journal_revoke_list *revoke_list, uint32_t *blknu);
static int ext2_journal_process_revoke_block(struct ext2fs_journal *jrnp,
    void *data, uint32_t sequence);
static bool
ext2_journal_is_block_revoked(struct ext2fs_journal_revoke_table *table,
    uint32_t blocknr, uint32_t sequence);
static int ext2_recover_orphan_list(struct ext2fs_journal *jrnp);
static int ext2_journal_walk_trans(struct ext2fs_journal *jrnp,
    enum ext2fs_journal_pass_type pass, uint32_t trans_start,
    uint32_t *next_trans_start, uint32_t *trans_seq);

/*
 * Verify if the given data block is a valid journal block.
 */
static bool
ext2_journal_verify_block(void *data)
{
	struct ext2fs_journal_block_header *jrn_bhr =
	    (struct ext2fs_journal_block_header *) data;
	return (be32toh(jrn_bhr->jbh_magic) == EXT2_JOURNAL_MAGIC);
}

static inline uint32_t
ext2_journal_next_block(struct ext2fs_journal *jrnp, uint32_t blknu)
{
	blknu++;
	if (blknu > jrnp->jrn_last)
		blknu = jrnp->jrn_first;

	return (blknu);
}

/*
 * Opens the journal inode and reads its superblock.
 *
 * Locate the journal indoe, read its first block (superblock), verify it,
 * and populate an in-memory copy of the journal superblock.
 *
 * The caller is responsible for releasing the vnode if the function succeeds
 * or an error occurs after vpp is set.
 *
 * The caller is responsible for freeing the journal superblock if the function
 * succeeds.
 */
static int
ext2_journal_open_inode(struct mount *mp, struct vnode **vpp,
    struct ext2fs_journal_sb **jrn_sbpp) {
	struct buf *jrn_buf;
	void *jrn_data;
	struct ext2mount *ump = VFSTOEXT2(mp);
	struct m_ext2fs *fs = ump->um_e2fs;
	struct ext2fs_journal_sb *jrn_sbp;
	uint32_t jrn_inum = fs->e2fs->e3fs_journal_inum;
	int error;

	/* Check if journal inode number is valid */
	if (jrn_inum == 0 ||
	    jrn_inum != EXT2_JOURNALINO) {
		EXT2_JERROR("invalid journal inode num: %u\n", jrn_inum);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	error = VFS_VGET(mp, EXT2_JOURNALINO, LK_EXCLUSIVE, vpp);
	if (error != 0) {
		*vpp = NULL;
		EXT2_JERROR("vfs_get failed: %d\n", error);
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	/* bufobj size must be initilized else panics */
	if ((*vpp)->v_bufobj.bo_bsize == 0) {
	    (*vpp)->v_bufobj.bo_bsize = fs->e2fs_bsize;
	}

	error = bread(*vpp, 0, (daddr_t) fs->e2fs_bsize, NOCRED, &jrn_buf);
	if (error != 0) {
		EXT2_JERROR("bread failed: %d\n", error);
		vput(*vpp);
		*vpp = NULL;
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	jrn_data = jrn_buf->b_data;
	if (!ext2_journal_verify_block(jrn_data)) {
		EXT2_JERROR("journal magic number mismatch\n");
		brelse(jrn_buf);
		vput(*vpp);
		*vpp = NULL;
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	jrn_sbp = (struct ext2fs_journal_sb *) jrn_data;
	EXT2_JPRINT_JSB(jrn_sbp);
	if (be32toh(jrn_sbp->jsb_header.jbh_blocktype) !=
	    EXT2_JOURNAL_FORMAT_BASIC &&
	    be32toh(jrn_sbp->jsb_header.jbh_blocktype) !=
	    EXT2_JOURNAL_FORMAT_EXTENDED)
	    {
		    EXT2_JERROR("journal is not the proper version\n");
		    brelse(jrn_buf);
		    vput(*vpp);
		    *vpp = NULL;
		    EXT2_JTRACE_EXIT(EINVAL);
		    return (EINVAL);
	    }

	*jrn_sbpp = (struct ext2fs_journal_sb *)
		malloc(sizeof(struct ext2fs_journal_sb), M_EXT2JSB, M_WAITOK);

	memcpy(*jrn_sbpp, jrn_sbp, sizeof(struct ext2fs_journal_sb));

	brelse(jrn_buf);
	VOP_UNLOCK(*vpp);

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static uint32_t
ext2_journal_block_type(void *data)
{
	struct ext2fs_journal_block_header *jrn_bhr =
	    (struct ext2fs_journal_block_header *) data;
	return (be32toh(jrn_bhr->jbh_blocktype));
}

/*
 * Calculate the size of the tags.
 *
 * I believe the sizes are:
 *
 * 8-bytes for 32-bit journal with same UUID.
 * 12-bytes for 64-bit journal with same UUID.
 * 24-bytes for 32-bit with same UUID.
 * 28-bytes for 64-bit journal when same UUID is not set.
 *
 * Assume 8-bytes for now.
 */
static uint32_t
ext2_journal_tag_size(struct ext2fs_journal_sb *jsbp)
{
	return (8);
}

static int
ext2_journal_parse_desc_blk(struct ext2fs_journal *jrnp, void *data,
    uint32_t blk_size, uint32_t *tag_count, bool *has_last_tag)
{
	struct ext2fs_journal_sb *jsb;
	struct ext2fs_journal_block_header *header;
	struct ext2fs_journal_desc_tag *tag;
	char *c_data = (char *)data;
	uint32_t blocknum_low;
	uint32_t stride;
	uint16_t flags;
	int data_index = 0;
	int max_size;

	EXT2_JTRACE_ENTER();


	jsb = jrnp->jrn_sb;
	header = (struct ext2fs_journal_block_header *)data;
	stride = ext2_journal_tag_size(jsb);
	*tag_count = 0;
	*has_last_tag = false;
	max_size = blk_size - sizeof(struct ext2fs_journal_block_header);

	EXT2_JPRINTF("desc block seq num: %u\n",
	    be32toh(header->jbh_sequence_num));
	/*
	 * Account for potential descriptor tail in checksum v2.
	 * Not used right now.
	 */
	if (jsb->jsb_feature_incompat & EXT2_JOURNAL_INCOMPAT_CHECKSUM_V2) {
		max_size -= sizeof(struct ext2fs_journal_desc_tail);
	}

	/* Skip past the block header */
	c_data += sizeof(struct ext2fs_journal_block_header);
	data_index = 0;

	while (data_index + stride <= max_size) {
		tag = (struct ext2fs_journal_desc_tag *)(&(c_data[data_index]));
		blocknum_low = be32toh(tag->jdt_blocknum_low);
		flags = be16toh(tag->jdt_flags);

		printf("desc blk: tag num: %d\n", *tag_count);
		printf("desc blk: tag flag: %u\n", flags);
		printf("desc blk: blocknum low: %u\n", blocknum_low);

		(*tag_count)++;
		if (flags & EXT2_JOURNAL_TAG_LAST_ENTRY) {
			*has_last_tag = true;
			break;
		}
		data_index += stride;
	}

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_walk_trans_tail(struct ext2fs_journal *jrnp,
    enum ext2fs_journal_pass_type pass, uint32_t start_blk,
    uint32_t expected_seq, uint32_t *next_trans_start)
{
	struct ext2fs_journal_block_header *header;
	struct vnode *vp;
	struct m_ext2fs *fs;
	struct buf *bp;
	void *block_data;
	uint32_t cur_blk;
	uint32_t block_type;
	int error = 0;

	bp = NULL;
	vp = jrnp->jrn_vp;
	fs = jrnp->jrn_fs;
	cur_blk = start_blk;
	for (;;) {
		error = bread(vp, cur_blk, (daddr_t)fs->e2fs_bsize, NOCRED,
		    &bp);
		if (error) {
			EXT2_JERROR("tail block read fail: %d\n", error);
			return (error);
		}

		block_data = bp->b_data;
		if (!ext2_journal_verify_block(block_data)) {
			brelse(bp);
			return (EINVAL);
		}

		header = (struct ext2fs_journal_block_header *)block_data;
		block_type = ext2_journal_block_type(block_data);

		if (be32toh(header->jbh_sequence_num) != expected_seq) {
			brelse(bp);
			return (EINVAL);
		}

		if (block_type == EXT2_JOURNAL_REVOKE_BLOCK) {
			if (pass == PASS_REVOKE) {
				error = ext2_journal_process_revoke_block(jrnp,
				    block_data, expected_seq);
				if (error) {
					brelse(bp);
					return (error);
				}
			}
			brelse(bp);
			cur_blk = ext2_journal_next_block(jrnp, cur_blk);

		} else if (block_type == EXT2_JOURNAL_COMMIT_BLOCK) {
			brelse(bp);
			*next_trans_start = ext2_journal_next_block(jrnp,
			    cur_blk);
			/* Transaction complete */
			return (0);

		} else {
			brelse(bp);
			return (EINVAL);
		}
	}
}

static int
ext2_journal_walk_trans(struct ext2fs_journal *jrnp,
    enum ext2fs_journal_pass_type pass, uint32_t trans_start,
    uint32_t *next_trans_start, uint32_t *trans_seq)
{
	struct vnode *vp;
	struct ext2fs_journal_desc_tag *tag;
	struct m_ext2fs *fs;
	struct buf *desc_buf = NULL;
	struct buf *jrn_data_buf = NULL;
	struct buf *real_data_buf = NULL;
	bool has_last_tag = false;
	bool more_desc_blocks = true;
	uint32_t cur_blk, jrn_blk_ptr, target_blknu;
	uint32_t tag_offset, tag_size, blocks_in_desc;
	uint32_t expected_seq = 0, cur_seq;
	char *desc_data;
	int error = 0;

	EXT2_JTRACE_ENTER();

	vp = jrnp->jrn_vp;
	fs = jrnp->jrn_fs;
	cur_blk = trans_start;
	tag_size = ext2_journal_tag_size(jrnp->jrn_sb);
	/* Walk all descriptor and data blocks for this transaction */
	while (more_desc_blocks) {
		error = bread(vp, cur_blk, (daddr_t)fs->e2fs_bsize, NOCRED,
		    &desc_buf);
		if (error) {
			EXT2_JERROR("desc block read fail: %d\n", error);
			return (error);
		}

		desc_data = desc_buf->b_data;
		if (!ext2_journal_verify_block(desc_data)) {
			brelse(desc_buf);
			return (EINVAL);
		}
		if (ext2_journal_block_type(desc_data) !=
		    EXT2_JOURNAL_DESCRIPTOR_BLOCK) {
			brelse(desc_buf);
			return (EINVAL);
		}
		cur_seq = be32toh(
		    ((struct ext2fs_journal_block_header *)desc_data)
			->jbh_sequence_num);
		if (trans_seq) {
			if (cur_blk == trans_start) {
				expected_seq = cur_seq;
				*trans_seq = cur_seq;
			} else {
				if (cur_seq != expected_seq) {
					EXT2_JERROR(
					    "Sequence mismatch in desc blocks:\
expected%u, got %u\n",
					    expected_seq, cur_seq);
					brelse(desc_buf);
					return (EINVAL);
				}
			}
		}
		/* Parse descriptor to get block count and last tag status */
		error = ext2_journal_parse_desc_blk(jrnp, desc_data,
		    jrnp->jrn_blocksize, &blocks_in_desc, &has_last_tag);
		if (error) {
			brelse(desc_buf);
			return (error);
		}

		more_desc_blocks = !has_last_tag;
		tag_offset = sizeof(struct ext2fs_journal_block_header);
		jrn_blk_ptr = ext2_journal_next_block(jrnp, cur_blk);
		/* Process all tags within this descriptor block */
		for (uint32_t i = 0; i < blocks_in_desc; i++) {
			tag = (struct ext2fs_journal_desc_tag *)(desc_data +
			    tag_offset);
			target_blknu = be32toh(tag->jdt_blocknum_low);
			if (pass == PASS_REPLAY) {
				if (ext2_journal_is_block_revoked(
					jrnp->jrn_revoke_table, target_blknu,
					cur_seq)) {
					/* Skip replaying this revoked block */
				} else {
					/* Read journal data block */
					error = bread(vp, jrn_blk_ptr,
					    jrnp->jrn_blocksize, NOCRED,
					    &jrn_data_buf);
					if (error)
						goto walk_fail;

					/* Get target block and copy data */
					real_data_buf = getblk(jrnp->jrn_devvp,
					    fsbtodb(fs, target_blknu),
					    jrnp->jrn_blocksize, 0, 0, 0);
					if (real_data_buf == NULL) {
						error = ENOMEM;
						brelse(jrn_data_buf);
						goto walk_fail;
					}

					memcpy(real_data_buf->b_data,
					    jrn_data_buf->b_data,
					    jrnp->jrn_blocksize);
					brelse(jrn_data_buf);
					jrn_data_buf = NULL;

					error = bwrite(real_data_buf);
					real_data_buf = NULL;
					if (error)
						goto walk_fail;
				}
			}
			jrn_blk_ptr = ext2_journal_next_block(jrnp,
			    jrn_blk_ptr);
			tag_offset += tag_size;
		}
		brelse(desc_buf);
		desc_buf = NULL;
		cur_blk = jrn_blk_ptr;
	}

	error = ext2_journal_walk_trans_tail(jrnp, pass, cur_blk, cur_seq,
	    next_trans_start);
walk_fail:
	if (desc_buf)
		brelse(desc_buf);
	if (jrn_data_buf)
		brelse(jrn_data_buf);
	if (real_data_buf)
		brelse(real_data_buf);
	return (error);
}

int
ext2_journal_recover(struct ext2fs_journal *jrnp)
{
	struct ext2mount *ump;
	struct m_ext2fs *fs;
	struct ext2fs_journal_sb *disk_sb;
	struct buf *jsb_buf;
	uint32_t start_block, end_block;
	uint32_t curr_trans_start, next_trans_start;
	uint32_t expected_seq, actual_seq;
	int error;

	EXT2_JTRACE_ENTER();

#if defined(DISABLE_RECOVERY)
	return (0);
#endif

	ump = jrnp->jrn_em;
	fs = ump->um_e2fs;
	jsb_buf = NULL;

	if (!(jrnp->jrn_flags & EXT2_JOURNAL_NEEDS_RECOVERY)) {
		EXT2_JPRINTF("Recovery not needed, journal is clean.\n");
		return (0);
	}

	/*
	 * Find the range of journal to recover.
	 */
	start_block = curr_trans_start = jrnp->jrn_log_start;
	end_block = start_block;
	expected_seq = jrnp->jrn_sequence;
	for (;;) {
		error = ext2_journal_walk_trans(jrnp, PASS_INITIAL,
		    curr_trans_start, &next_trans_start, &actual_seq);
		/* TODO better error handling for bread fails */
		if (error) {
			/* This is expected for initial pass. */
			error = 0;
			break;
		}
		if (actual_seq != expected_seq) {
			/* Consider seq num wraparound. */
			if (expected_seq == 0xFFFFFFFF && actual_seq == 1) {
				expected_seq = actual_seq;
			} else {
				break;
			}
		}

		end_block = next_trans_start;
		expected_seq++;
		if (next_trans_start == start_block) {
			/* We have wrapped around the entire journal */
			break;
		}
		curr_trans_start = next_trans_start;
	}

	if (start_block == end_block) {
		EXT2_JPRINTF("No transaction log records to recover.\n");
		jrnp->jrn_log_start = jrnp->jrn_first;
		jrnp->jrn_sequence = 1;
		jrnp->jrn_flags &= ~EXT2_JOURNAL_NEEDS_RECOVERY;
		jrnp->jrn_flags |= EXT2_JOURNAL_CLEAN;

		return (0);
	}
	/*
	 * Walk the journal range and build the revoke table.
	 */
	curr_trans_start = start_block;
	while (curr_trans_start != end_block) {
		error = ext2_journal_walk_trans(jrnp, PASS_REVOKE,
		    curr_trans_start, &next_trans_start, NULL);
		if (error)
			goto fail;
		curr_trans_start = next_trans_start;
	}

	/*
	 * Walk the range again, replaying all non-revoked blocks.
	 */
	curr_trans_start = start_block;
	while (curr_trans_start != end_block) {
		error = ext2_journal_walk_trans(jrnp, PASS_REPLAY,
		    curr_trans_start, &next_trans_start, NULL);
		if (error)
			goto fail;
		curr_trans_start = next_trans_start;
	}

	jrnp->jrn_log_start = next_trans_start;
	jrnp->jrn_log_end = next_trans_start;
	jrnp->jrn_sequence = expected_seq;

	/* Write the updated journal superblock to disk */
	error = bread(jrnp->jrn_vp, 0, jrnp->jrn_blocksize, NOCRED, &jsb_buf);
	if (error) {
		goto fail;
	}

	/* Write the superblock back to disk */
	disk_sb = (struct ext2fs_journal_sb *)jsb_buf->b_data;
	disk_sb->jsb_start_block_num = htobe32(next_trans_start);
	disk_sb->jsb_sequence_id = htobe32(expected_seq);
	disk_sb->jsb_errno = htobe32(0); /* Clear any error status */
	error = bwrite(jsb_buf);
	jsb_buf = NULL;
	if (error) {
		goto fail;
	}

	/* Is this the right flag to mark the journal as clean? */
	fs->e2fs->e2fs_state = htole16(
	    (le16toh(fs->e2fs->e2fs_state) | E2FS_ISCLEAN));
	error = ext2_sbupdate(ump, 1);
	if (error != 0) {
		goto fail;
	}

	jrnp->jrn_flags &= ~EXT2_JOURNAL_NEEDS_RECOVERY;
	jrnp->jrn_flags |= EXT2_JOURNAL_CLEAN;

	/* Recovering orphan list will be journaled. */
	error = ext2_recover_orphan_list(jrnp);
	if (error)
		goto fail;

	EXT2_JPRINTF("JOURNAL RECOVERY SUCCESS\n");
	return (0);

fail:
	if (jsb_buf)
		brelse(jsb_buf);
	jrnp->jrn_flags |= EXT2_JOURNAL_ABORTED;
	EXT2_JERROR("Journal recovery failed, needs fsck. error: %d\n", error);
	return (error);
}

/*
 * Initialize the in-memory journal structure.
 *
 * Populate the ext2fs_journal structure with useful parameters from the
 * on-disk journal superblock and filesystem state.
 */
static int
ext2_journal_init(struct ext2fs_journal *jrnp)
{
	struct ext2fs_journal_sb *disk_sb = jrnp->jrn_sb;

	EXT2_JTRACE_ENTER();

	mtx_init(&jrnp->jrn_lock, "ext2jrnl", NULL, MTX_DEF);
	cv_init(&jrnp->jrn_trans_commit_cv, "ext2jrn_commit_cv");
	cv_init(&jrnp->jrn_trans_start_cv, "ext2jrn_start_cv");
	cv_init(&jrnp->jrn_sync_cv, "ext2jrn_sync_cv");

	jrnp->jrn_active_trans = NULL;
	jrnp->jrn_committing_trans = NULL;
	TAILQ_INIT(&jrnp->jrn_checkpoint_list);
	jrnp->jrn_revoke_table = ext2_journal_revoke_table_create();

	jrnp->jrn_sequence = be32toh(disk_sb->jsb_sequence_id);
	jrnp->jrn_blocksize = be32toh(disk_sb->jsb_blocksize);
	jrnp->jrn_max_blocks = be32toh(disk_sb->jsb_max_blocks);
	jrnp->jrn_first = be32toh(disk_sb->jsb_first_block);
	jrnp->jrn_last = jrnp->jrn_first + jrnp->jrn_max_blocks - 1;

	jrnp->jrn_log_start = be32toh(disk_sb->jsb_start_block_num);
	if (jrnp->jrn_log_start == 0) {
		jrnp->jrn_log_start = jrnp->jrn_first;
	}

	jrnp->jrn_free_blocks = jrnp->jrn_max_blocks;
	jrnp->jrn_log_end = jrnp->jrn_log_start; /* Start with end at the same place */

	if (jrnp->jrn_max_blocks < EXT2_JOURNAL_MIN_BLOCKS) {
		EXT2_JERROR("journal number of blocks too little\n");
		mtx_destroy(&jrnp->jrn_lock);
		cv_destroy(&jrnp->jrn_trans_commit_cv);
		cv_destroy(&jrnp->jrn_trans_start_cv);
		cv_destroy(&jrnp->jrn_sync_cv);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	/* Initialize journal state */
	if (le16toh(jrnp->jrn_fs->e2fs->e2fs_state) & E2FS_ISCLEAN) {
		jrnp->jrn_flags |= EXT2_JOURNAL_CLEAN;
	} else {
		jrnp->jrn_flags |= EXT2_JOURNAL_NEEDS_RECOVERY;
	}

	EXT2_JTRACE_EXIT(0);
	return (0);
}

/*
 * Closes the journal and releases resources.
 */
int
ext2_journal_close(struct ext2fs_journal *jrnp)
{
	KASSERT(jrnp->jrn_active_trans == NULL,
	    ("journal close while active trans\n"));
	KASSERT(jrnp->jrn_committing_trans == NULL,
	    ("journal close while comitting trans\n"));
	if (jrnp == NULL)
		return (0);

	if (jrnp->jrn_vp != NULL) {
		vrele(jrnp->jrn_vp);
	}

	if (jrnp->jrn_sb != NULL)
		free(jrnp->jrn_sb, M_EXT2JSB);

	if (jrnp->jrn_revoke_table != NULL)
		ext2_journal_revoke_table_destroy(jrnp->jrn_revoke_table);

	mtx_destroy(&jrnp->jrn_lock);
	cv_destroy(&jrnp->jrn_trans_commit_cv);
	cv_destroy(&jrnp->jrn_trans_start_cv);
	cv_destroy(&jrnp->jrn_sync_cv);

	jrnp->jrn_em->um_journal = NULL;

	free(jrnp, M_EXT2JOURNAL);
	return (0);
}

/*
 * Reads on-disk journal and initializes in-memory journal.
 *
 * Main entry point for journal initilization. It allocates the primary journal
 * structure, opens the journal inode, reads and validates the journal
 * superblock, and intializes journal parameters.
 */
int
ext2_journal_open(struct mount *mp, struct ext2fs_journal **jrnpp)
{
	struct ext2mount *ump = VFSTOEXT2(mp);
	struct m_ext2fs *fs = ump->um_e2fs;
	int error;

	EXT2_JTRACE_ENTER();

	*jrnpp = malloc(sizeof(struct ext2fs_journal), M_EXT2JOURNAL,
	    M_WAITOK | M_ZERO);
	(*jrnpp)->jrn_devvp = ump->um_devvp;
	error = ext2_journal_open_inode(mp, &((*jrnpp)->jrn_vp),
                              &((*jrnpp)->jrn_sb));
	if (error != 0) {
		EXT2_JERROR("failed to open journal inode. error: %d\n", error);
		goto fail_sb;
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	(*jrnpp)->jrn_fs = fs;
	(*jrnpp)->jrn_em = ump;
	error = ext2_journal_init(*jrnpp);
	if (error != 0) {
		EXT2_JERROR("failed initialize journal. error: %d\n", error);
		goto fail_init;
		EXT2_JTRACE_EXIT(error);
		return (error);
	}
	ump->um_journal = *jrnpp;

	EXT2_JTRACE_EXIT(0);
	return (0);

fail_init:
	vput((*jrnpp)->jrn_vp);
	(*jrnpp)->jrn_vp = NULL;
	free((*jrnpp)->jrn_sb, M_EXT2JSB);
fail_sb:
	free(*jrnpp, M_EXT2JOURNAL);
	*jrnpp = NULL;

	return (error);
}

/*
 * Allocate and initialize a journal buffer.
 */
static struct ext2fs_journal_buf *
ext2_journal_buf_alloc(struct ext2fs_journal *jrnp, struct buf *bp,
    enum ext2fs_journal_buf_type type)
{
	struct ext2fs_journal_buf *jbuf;

	EXT2_JTRACE_ENTER();

	jbuf = malloc(sizeof(struct ext2fs_journal_buf), M_EXT2JBUF,
	    M_WAITOK | M_ZERO);

	jbuf->jb_owning_trans = jrnp->jrn_active_trans;
	jbuf->jb_buf = bp;
	jbuf->jb_type = type;
	jbuf->jb_blocknr = bp->b_blkno;
	jbuf->jb_revoked = false;

	/* Faster way to find the jbuf */
	bp->b_fsprivate1 = jbuf;

	EXT2_JTRACE_EXIT(0);
	return (jbuf);
}

/*
 * Free a journal buf type.
 */
static void
ext2_journal_buf_free(struct ext2fs_journal_buf *jbuf)
{
	KASSERT(jbuf != NULL, ("jbuf to free is NULL\n"));
	KASSERT(jbuf->jb_buf == NULL, ("buf of jbuf is NOT NULL\n"));
	EXT2_JTRACE_ENTER();

	free(jbuf, M_EXT2JBUF);
}

/*
 * Free all jbufs in a list.
 */
static void
ext2_journal_buf_free_list(struct ext2_journal_buf_list *head)
{
	struct ext2fs_journal_buf *jbuf, *next;

	EXT2_JTRACE_ENTER();

	TAILQ_FOREACH_SAFE(jbuf, head, jb_list, next) {
		TAILQ_REMOVE(head, jbuf, jb_list);
		ext2_journal_buf_free(jbuf);
	}
}

static struct ext2fs_journal_buf *
ext2_journal_find_jbuf(struct ext2fs_journal_transaction *trans,
    uint32_t blocknr)
{
	struct ext2fs_journal_buf *jbuf;
	TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
		if (jbuf->jb_blocknr == blocknr)
			return (jbuf);
	}
	return (NULL);
}

/*
 * Allocate and initialize a transaction.
 */
static struct ext2fs_journal_transaction *
ext2_journal_transaction_alloc(struct ext2fs_journal *journal)
{
	struct ext2fs_journal_transaction *trans;

	EXT2_JTRACE_ENTER();

	trans = malloc(sizeof(struct ext2fs_journal_transaction), M_EXT2JTRANS,
	    M_NOWAIT | M_ZERO);

	trans->jt_journal = journal;
	trans->jt_state = EXT2_TRANS_RUNNING;
	trans->jt_refcount = 0;
	trans->jt_owner = NULL;
	trans->jt_blocks_used = 0;
	trans->jt_blocks_reserved = 0;
	trans->jt_data_count = 0;
	trans->jt_metadata_count = 0;

	TAILQ_INIT(&trans->jt_metadata_buffers);
	TAILQ_INIT(&trans->jt_revoke_list);

	return (trans);
}

/*
 * Free a transaction and all associated resources.
 */
static void
ext2_journal_transaction_free(struct ext2fs_journal_transaction *trans)
{

	EXT2_JTRACE_ENTER();

	if (trans == NULL)
		return;
	/* Free all journal buffer descriptors */
	ext2_journal_buf_free_list(&trans->jt_metadata_buffers);
	ext2_journal_revoke_list_clear(&trans->jt_revoke_list);

	free(trans, M_EXT2JTRANS);
}

/*
 * Mark a data jbuf as dirty.
 */
int
ext2_journal_dirty_data(struct ext2fs_journal *jrnp, struct buf *bp)
{
	struct ext2fs_journal_transaction *trans;
	struct ext2fs_journal_buf *jbuf;

	EXT2_JTRACE_ENTER();

	EXT2_JLOCK(jrnp);
	trans = jrnp->jrn_active_trans;

	if (trans == NULL || trans->jt_owner != curthread) {
		EXT2_JUNLOCK(jrnp);
		return (EINVAL);
	}

	/* Check if buffer is already journaled in this trans. */
	jbuf = (struct ext2fs_journal_buf *) bp->b_fsprivate1;
	if (jbuf != NULL) {
		/*
		 * If writing data to revoked block this transaction,
		 * cancel revoke.
		* */
		if (jbuf->jb_revoke_entry != NULL)
			ext2_journal_cancel_revoke(trans, jbuf);
		jbuf->jb_buf = NULL;
		bp->b_fsprivate1 = NULL;
		ext2_journal_buf_free(jbuf);
	}
	trans->jt_data_count++;
	EXT2_JUNLOCK(jrnp);
	EXT2_JTRACE_EXIT(0);
	return (0);
}


/*
 * Mark a data jbuf as dirty.
 *
 * We need to ensure the buffer cache does not write our data to disk or evict
 * our buffer.
 */
int
ext2_journal_dirty_metadata(struct ext2fs_journal *jrnp, struct buf *bp)
{
	struct ext2fs_journal_transaction *trans;
	struct ext2fs_journal_buf *jbuf;

	EXT2_JTRACE_ENTER();

	EXT2_JLOCK(jrnp);
	trans = jrnp->jrn_active_trans;

	if (trans == NULL || trans->jt_owner != curthread) {
		EXT2_JPRINTF("trans null or not current thread\n");
		EXT2_JUNLOCK(jrnp);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	jbuf = (struct ext2fs_journal_buf *) bp->b_fsprivate1;
	if (jbuf != NULL && jbuf->jb_owning_trans == trans) {
		if (jbuf->jb_revoke_entry != NULL)
			ext2_journal_cancel_revoke(trans, jbuf);

		EXT2_JUNLOCK(jrnp);
		bqrelse(bp);
		return(0);
	}

	/*
	 * Since only some parts of the filesystem is journaled, the passed in
	 * metadata buffer can be in multiple states as of right now.
	 *
	 * Eventually, all passed in bufs should not be dirty but this is not
	 * guaranteed right now.
	 *
	 * For now we will have a bunch of checks on the current buf state
	 * for debugging and sanity checks.
	 */
	EXT2_JPRINT_BUF(bp);

	// TODO undirty buffer?
	if (bp->b_flags & B_DELWRI) {
		EXT2_JPRINTF("dirty buf\n");
	}

	switch (bp->b_qindex) {
	case QUEUE_NONE:
		EXT2_JPRINTF("buf not on freelist\n");
		break;
	case QUEUE_EMPTY:
		EXT2_JPRINTF("buf on empty list\n");
		break;
	case QUEUE_DIRTY:
		EXT2_JPRINTF("buf on dirty list\n");
		break;
	case QUEUE_CLEAN:
		EXT2_JPRINTF("buf on clean list\n");
		break;
	case QUEUE_SENTINEL:
		EXT2_JPRINTF("buf on sentinel list\n");
		break;
	default:
		EXT2_JPRINTF("buf on invalid index: %u\n", bp->b_qindex);
		break;
	}

	if (bp->b_qindex != QUEUE_NONE) {
		EXT2_JPRINTF("Force remove buf from freelist\n");
		bremfreef(bp);
	}

	EXT2_JUNLOCK(jrnp);
	jbuf = ext2_journal_buf_alloc(jrnp, bp, EXT2_JBUF_METADATA);
	EXT2_JLOCK(jrnp);

	EXT2_JPRINTF("new jbuf created\n");

	/* Notifies the buffer cache we are doing our own management */
	bp->b_flags |= B_MANAGED;

	TAILQ_INSERT_TAIL(&trans->jt_metadata_buffers, jbuf, jb_list);
	trans->jt_metadata_count++;
	EXT2_JPRINTF("new jbuf added to metadata list\n");

	/* Unlocks the buf */
	bqrelse(bp);

	EXT2_JUNLOCK(jrnp);
	EXT2_JTRACE_EXIT(0);
	return (0);
}

/*
 * Starts a transaction or joins the current transaction if a nested operation.
 *
 * Journal is kept serial for now so only the same thread that started the
 * transaction can rejoin the current active transaction. This is to ensure
 * nested operations work.
 */
int
ext2_journal_start(struct ext2fs_journal *jrnp, int nblocks)
{
	struct ext2fs_journal_transaction *trans;
	struct thread *td = curthread;
	int required_blocks;
	/*
	 * We need blocks for normal fs blocks and
	 * descriptor and commit block for journal.
	 */
	required_blocks = nblocks + 2;

	EXT2_JTRACE_ENTER();
	EXT2_JLOCK(jrnp);
	for (;;) {
		trans = jrnp->jrn_active_trans;

		/* Another file op is active. */
		if (trans && trans->jt_refcount > 0) {
			cv_wait(&jrnp->jrn_trans_start_cv, &jrnp->jrn_lock);
			continue;
		}

		/* A transaction is commiting, wait. */
		if (jrnp->jrn_committing_trans != NULL) {
			cv_wait(&jrnp->jrn_trans_commit_cv, &jrnp->jrn_lock);
			continue;
		}

		/* Checkpoint to free up space and start a new transaction. */
		if (required_blocks > jrnp->jrn_free_blocks) {
			KASSERT(!TAILQ_EMPTY(&jrnp->jrn_checkpoint_list),
			    ("need space, but nothing to free"));

			if (trans != NULL) {
				jrnp->jrn_committing_trans = trans;
				jrnp->jrn_active_trans = NULL;
			}

			EXT2_JUNLOCK(jrnp);
			if (jrnp->jrn_committing_trans)
				ext2_journal_commit_trans(jrnp);
			ext2_journal_checkpoint_trans(jrnp);
			EXT2_JLOCK(jrnp);

			continue;
		}
		break;
	}

	if (trans != NULL) {
		trans->jt_owner = td;
		trans->jt_refcount = 1;
		trans->jt_blocks_reserved += required_blocks;
		jrnp->jrn_free_blocks -= required_blocks;
	} else {
		trans = ext2_journal_transaction_alloc(jrnp);
		trans->jt_owner = td;
		trans->jt_refcount = 1;
		trans->jt_blocks_reserved = required_blocks;
		jrnp->jrn_free_blocks -= required_blocks;
		jrnp->jrn_active_trans = trans;
	}

	EXT2_JUNLOCK(jrnp);
	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_write_desc_blocks(struct ext2fs_journal *jrnp, uint32_t *blknu)
{
	struct m_ext2fs *fs = jrnp->jrn_fs;
	struct ext2fs_journal_transaction *trans = jrnp->jrn_committing_trans;
	struct ext2fs_journal_block_header *header;
	struct ext2fs_journal_desc_tag *tag;
	struct ext2fs_journal_buf *jbuf;
	struct buf *desc_buf = NULL;
	char *desc_data;
	uint32_t tag_offset;
	uint32_t tag_size = ext2_journal_tag_size(jrnp->jrn_sb);
	uint32_t max_tags_per_block;
	uint32_t tags_in_current_block = 0;
	int error = 0;
	bool need_new_desc_block = true;

	EXT2_JTRACE_ENTER();

	/* Calculate maximum tags per descriptor block */
	max_tags_per_block = (jrnp->jrn_blocksize -
	    sizeof(struct ext2fs_journal_block_header));
	max_tags_per_block /= tag_size;

	TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
		/* Check if we need a new descriptor block */
		if (need_new_desc_block) {
			/* Write previous descriptor block if exists */
			if (desc_buf != NULL) {
				error = bwrite(desc_buf);
				if (error) {
					EXT2_JERROR("bwrite fail: %d\n", error);
					EXT2_JTRACE_EXIT(error);
					return (error);
				}
				*blknu = ext2_journal_next_block(jrnp, *blknu);
			}

			/* Get new descriptor block */
			EXT2_JPRINTF(
			    "Writing new desc block to journal offset: %u\n",
			    *blknu);
			desc_buf = getblk(jrnp->jrn_vp, *blknu,
			    jrnp->jrn_blocksize, 0, 0, 0);
			if (desc_buf == NULL) {
				EXT2_JERROR("getblk failed\n");
				EXT2_JTRACE_EXIT(ENOMEM);
				return (ENOMEM);
			}

			desc_data = desc_buf->b_data;
			memset(desc_data, 0, jrnp->jrn_blocksize);

			/* Write header */
			header = (struct ext2fs_journal_block_header *)
			    desc_data;
			header->jbh_magic = htobe32(EXT2_JOURNAL_MAGIC);
			header->jbh_blocktype = htobe32(
			    EXT2_JOURNAL_DESCRIPTOR_BLOCK);
			header->jbh_sequence_num = htobe32(jrnp->jrn_sequence);

			tag_offset = sizeof(struct ext2fs_journal_block_header);
			tags_in_current_block = 0;
			need_new_desc_block = false;
		}

		tag = (struct ext2fs_journal_desc_tag *)(desc_data +
		    tag_offset);

		uint16_t current_flags = EXT2_JOURNAL_TAG_SAME_UUID;

		/* Check if this is the last tag in the transaction */
		if (TAILQ_NEXT(jbuf, jb_list) == NULL) {
			current_flags |= EXT2_JOURNAL_TAG_LAST_ENTRY;
		}

		EXT2_JPRINTF("tagging block: %u, flags: 0x%x\n",
			     dbtofsb(fs, jbuf->jb_blocknr), current_flags);

		/* write logical block nu to journal */
		tag->jdt_blocknum_low = htobe32(dbtofsb(fs, jbuf->jb_blocknr));
		tag->jdt_flags = htobe16(current_flags);
		tag->jdt_checksum = 0; /* TODO: implement checksums */

		tag_offset += tag_size;
		tags_in_current_block++;

		/*
		 * Check if we've filled this descriptor block.
		 */
		if (tags_in_current_block >= max_tags_per_block) {
			/* If not last tag, needs more desc block */
			if (TAILQ_NEXT(jbuf, jb_list) != NULL) {
				need_new_desc_block = true;
			}
		}
	}

	/* Write the final descriptor block */
	if (desc_buf != NULL) {
		error = bwrite(desc_buf);
		if (error) {
			EXT2_JERROR("bwrite fail: %d\n", error);
			EXT2_JTRACE_EXIT(error);
			return (error);
		}
		*blknu = ext2_journal_next_block(jrnp, *blknu);
	}

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_write_commit_blk(struct ext2fs_journal *jrnp, uint32_t *blknu)
{
	struct ext2fs_journal_commit_header *header;
	struct buf *commit_bp;
	int error;

	EXT2_JTRACE_ENTER();

	commit_bp = getblk(jrnp->jrn_vp, *blknu, jrnp->jrn_blocksize,
	    0, 0, 0);
	if (commit_bp == NULL) {
		EXT2_JERROR("getblk failed\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	/* Clear buffer completely */
	memset(commit_bp->b_data, 0, jrnp->jrn_blocksize);

	/* Write commit header in big-endian */
	header = (struct ext2fs_journal_commit_header *) commit_bp->b_data;
	header->jch_header.jbh_magic = htobe32(EXT2_JOURNAL_MAGIC);
	header->jch_header.jbh_blocktype = htobe32(EXT2_JOURNAL_COMMIT_BLOCK);
	header->jch_header.jbh_sequence_num = htobe32(jrnp->jrn_sequence);

	error = bwrite(commit_bp);
	if (error) {
		EXT2_JERROR("bwrite failed: %d\n", error);
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	*blknu = ext2_journal_next_block(jrnp, *blknu);

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_write_metadata_blcks(struct ext2fs_journal *jrnp,
    struct ext2fs_journal_transaction *trans, uint32_t *blknu)
{
	struct ext2fs_journal_buf *jbuf;
	struct buf *disk_jbuf;
	int error = 0;

	EXT2_JTRACE_ENTER();

	/* Write metadata blocks to journal */
	TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
		disk_jbuf = getblk(jrnp->jrn_vp, *blknu, jrnp->jrn_blocksize,
		    0, 0, 0);
		if (disk_jbuf == NULL) {
			EXT2_JERROR("getblk failed for metadata block\n");
			error = ENOMEM;
			break;
		}

		/* Copy metadata to journal buffer */
		memcpy(disk_jbuf->b_data, jbuf->jb_buf->b_data, jrnp->jrn_blocksize);

		error = bwrite(disk_jbuf);
		if (error) {
			EXT2_JERROR("bwrite failed for metadata: %d\n", error);
			break;
		}

		*blknu = ext2_journal_next_block(jrnp, *blknu);
	}

	EXT2_JTRACE_EXIT(error);
	return (error);
}


static int
ext2_journal_checkpoint_metadata(struct ext2fs_journal *jrnp,
    struct ext2fs_journal_transaction *trans)
{
	struct ext2fs_journal_buf *jbuf;
	struct buf *bp;
	int error = 0;

	EXT2_JTRACE_ENTER();
	TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
		bp = jbuf->jb_buf;
		error = BUF_LOCK(bp, LK_EXCLUSIVE, NULL);
		if (error) {
			EXT2_JERROR(
			    "failed to lock the buf before brelse: %d\n",
			    error);
			return (error);
		}
		bp->b_flags &= ~B_MANAGED;
#if defined(ENABLE_CHECKPOINT_WRITE)
		/* Write buf to real disk if not revoked */
		if (!jbuf->jb_revoked) {
			error = bwrite(bp);
			if (error) {
				brelse(bp);
				jbuf->jb_buf = NULL;
				EXT2_JERROR("checkpoing write failed: %d\n",
				    error);
				return (error);
			}
		} else {
			bp->b_flags |= B_INVAL;
			brelse(bp);
		}
#else
		bp->b_flags |= B_INVAL;
		brelse(bp);
#endif
		jbuf->jb_buf = NULL;
	}

	EXT2_JTRACE_EXIT(error);
	return (error);
}

int
ext2_journal_checkpoint_trans(struct ext2fs_journal *jrnp)
{
	struct ext2fs_journal_transaction *trans, *next_trans;
#if defined(ENABLE_CHECKPOINT_WRITE)
	struct ext2fs_journal_sb *disk_sb;
	struct buf *sb_buf;
#endif
	int freed_blocks = 0;
	int error = 0;

	EXT2_JTRACE_ENTER();
	EXT2_JLOCK(jrnp);
	/*
	 * If there's nothing to checkpoint, exit.
	 */
	if (TAILQ_EMPTY(&jrnp->jrn_checkpoint_list)) {
		goto unlock_and_exit;
	}

	TAILQ_FOREACH_SAFE(trans, &jrnp->jrn_checkpoint_list,
	    jt_checkpoint_entry, next_trans) {
		if (trans->jt_refcount > 0) {
			//major error;
			EXT2_JERROR("transaction is still referenced\n");
			EXT2_JUNLOCK(jrnp);
			EXT2_JTRACE_EXIT(EINVAL);
			return (EINVAL);
		}

		error = ext2_journal_checkpoint_metadata(jrnp, trans);
		if (error) {
			EXT2_JERROR("checkpoint metadata failed\n");
			EXT2_JUNLOCK(jrnp);
			EXT2_JTRACE_EXIT(EINVAL);
			return (EINVAL);
		}

		TAILQ_REMOVE(&jrnp->jrn_checkpoint_list, trans,
		    jt_checkpoint_entry);
		freed_blocks += trans->jt_blocks_reserved;

		ext2_journal_transaction_free(trans);
	}

	if (freed_blocks > 0) {
		jrnp->jrn_free_blocks += freed_blocks;
		/* Update the log start and starting seq num */
		if (TAILQ_EMPTY(&jrnp->jrn_checkpoint_list)) {
			jrnp->jrn_log_start = jrnp->jrn_log_end;

#if defined(ENABLE_CHECKPOINT_WRITE)
			jrnp->jrn_sb->jsb_sequence_id = htobe32(
			    jrnp->jrn_sequence);
#endif
		}

#if defined(ENABLE_CHECKPOINT_WRITE)
		/*
		 * Update the on-disk journal superblock to make the free space
		 * persistent across reboots.
		 */
		error = bread(jrnp->jrn_vp, 0, jrnp->jrn_blocksize, NOCRED,
		    &sb_buf);
		if (error) {
			EXT2_JERROR("bread failed for journal superblock: %d\n",
			    error);
			jrnp->jrn_flags |= EXT2_JOURNAL_ABORTED;
			goto unlock_and_exit;
		}

		disk_sb = (struct ext2fs_journal_sb *)sb_buf->b_data;
		disk_sb->jsb_start_block_num = htobe32(jrnp->jrn_log_start);
		disk_sb->jsb_sequence_id = htobe32(
		    be32toh(jrnp->jrn_sb->jsb_sequence_id));

		error = bwrite(sb_buf);
		if (error) {
			EXT2_JERROR(
			    "bwrite failed for journal superblock: %d\n",
			    error);
			jrnp->jrn_flags |= EXT2_JOURNAL_ABORTED;
		}
#endif
	}

unlock_and_exit:
	KASSERT(jrnp->jrn_block_new_trans == true,
	    ("new transactions were allowed to start while checkpointing\n"));

	ext2_journal_revoke_table_clear(jrnp->jrn_revoke_table);
	EXT2_JUNLOCK(jrnp);
	EXT2_JTRACE_EXIT(0);
	return (0);
}

int
ext2_journal_commit_trans(struct ext2fs_journal *jrnp)
{
	struct ext2fs_journal_transaction *trans;
	uint32_t jrn_blknu;
	int error = 0;

	EXT2_JTRACE_ENTER();
	EXT2_JLOCK(jrnp);
	trans = jrnp->jrn_committing_trans;
	if (trans == NULL) {
		EXT2_JUNLOCK(jrnp);
		EXT2_JERROR("trans to commit is NULL\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	EXT2_JPRINT_TRANS(trans);
	EXT2_JPRINT_TRANS_BUFFERS(trans);
	trans->jt_state = EXT2_TRANS_COMMIT;


	jrn_blknu = jrnp->jrn_log_end;
	EXT2_JUNLOCK(jrnp);
	if (trans->jt_metadata_count > 0) {
		/* Write descriptor block */
		error = ext2_journal_write_desc_blocks(jrnp, &jrn_blknu);
		if (error) {
			EXT2_JPRINTF("write desc blk failed\n");
			EXT2_JERROR("Failed to write descriptor block: %d\n",
			    error);
			goto cleanup;
		}

		/* Write metadata blocks to journal */
		error = ext2_journal_write_metadata_blcks(jrnp, trans,
		    &jrn_blknu);
		if (error) {
			EXT2_JERROR("Failed to write metadata blocks: %d\n",
			    error);
			goto cleanup;
		}

		/* Write revoke block */
		error = ext2_journal_write_revoke_block(jrnp,
		    &trans->jt_revoke_list, &jrn_blknu);
		if (error) {
			EXT2_JERROR("Failed to write revoke block: %d\n",
			    error);
			goto cleanup;
		}

		/* Write commit block */
		error = ext2_journal_write_commit_blk(jrnp, &jrn_blknu);
		if (error) {
			EXT2_JERROR("Failed to write commit block: %d\n",
			    error);
			goto cleanup;
		}

		EXT2_JLOCK(jrnp);
		/* Update journal state */
		jrnp->jrn_sequence++;
		jrnp->jrn_log_end = jrn_blknu;
		EXT2_JUNLOCK(jrnp);

		EXT2_JPRINTF("Commit completed successfully at block: %u\n",
		    jrn_blknu);
	}

	EXT2_JLOCK(jrnp);
	/* Move committed transaction to checkpoint queue */
	TAILQ_INSERT_TAIL(&jrnp->jrn_checkpoint_list, trans,
	    jt_checkpoint_entry);
	jrnp->jrn_committing_trans = NULL;
	jrnp->jrn_sync = false;

	/* Wake up threads waiting on commit */
	/* Wake up threads waiting on start and commit signal. */
	cv_broadcast(&jrnp->jrn_trans_start_cv);
	cv_broadcast(&jrnp->jrn_trans_commit_cv);
	EXT2_JUNLOCK(jrnp);

	EXT2_JTRACE_EXIT(0);
	return (0);
cleanup:
	EXT2_JERROR("Fatal journal commit error: %d\n", error);

	jrnp->jrn_flags |= EXT2_JOURNAL_ABORTED;
	jrnp->jrn_committing_trans = NULL;

	cv_broadcast(&jrnp->jrn_trans_start_cv);
	EXT2_JUNLOCK(jrnp);

	ext2_journal_transaction_free(trans);

	EXT2_JTRACE_EXIT(error);
	return (error);
}

/*
 * Commit the transaction if all operations have completed. Else just decrement
 * refcount.
 */
int
ext2_journal_stop(struct ext2fs_journal *jrnp)
{
	struct ext2fs_journal_transaction *trans;
	struct thread *td = curthread;

	EXT2_JTRACE_ENTER();

	if (!EXT2_JACTIVE(jrnp))
		return (0);

	EXT2_JLOCK(jrnp);
	trans = jrnp->jrn_active_trans;

	if (trans == NULL || trans->jt_owner != td) {
		EXT2_JUNLOCK(jrnp);
		return (EINVAL);
	}

	trans->jt_refcount--;
	if (trans->jt_refcount == 0) {
		if (jrnp->jrn_sync) {
			/* Sync/fsync was called, send signal to commit */
			cv_broadcast(&jrnp->jrn_sync_cv);
		} else {
			/* Send signal to start new file op. */
			cv_broadcast(&jrnp->jrn_trans_start_cv);
		}
	}

	EXT2_JUNLOCK(jrnp);
	EXT2_JTRACE_EXIT(0);
	return (0);
}

/*
 * Force a journal commit.
 */
int
ext2_journal_force_commit(struct ext2fs_journal *jrnp)
{
	struct ext2fs_journal_transaction *trans;


	EXT2_JLOCK(jrnp);
	/* Wait for active atomic operation to finish */
	while ((trans = jrnp->jrn_active_trans) != NULL &&
	    trans->jt_refcount > 0) {
		cv_wait(&jrnp->jrn_trans_start_cv, &jrnp->jrn_lock);
	}

	/* Commit the transaction that just finished */
	if (trans && jrnp->jrn_committing_trans == NULL) {
		jrnp->jrn_committing_trans = trans;
		// TODO
		// jrnp->jrn_active_trans = NULL;
		EXT2_JUNLOCK(jrnp);
		return (ext2_journal_commit_trans(jrnp));
	}
	EXT2_JUNLOCK(jrnp);
	return (0);
}

static inline int
ext2_journal_revoke_hash(uint32_t blocknr)
{
	return (blocknr % EXT2_REVOKE_TABLE_SIZE);
}

static struct ext2fs_journal_revoke_table *
ext2_journal_revoke_table_create(void)
{
	struct ext2fs_journal_revoke_table *table;
	int i;

	table = malloc(sizeof(struct ext2fs_journal_revoke_table),  M_EXT2REVOKE,
	    M_WAITOK | M_ZERO);

	for (i = 0; i < EXT2_REVOKE_TABLE_SIZE; i++) {
		LIST_INIT(&table->jrt_hash[i]);
	}
	table->rt_record_count = 0;

	return (table);
}

static void
ext2_journal_revoke_table_destroy(struct ext2fs_journal_revoke_table *table)
{
	struct ext2fs_journal_revoke_record *record, *next;
	int i;

	if (table == NULL)
		return;

	for (i = 0; i < EXT2_REVOKE_TABLE_SIZE; i++) {
		LIST_FOREACH_SAFE(record, &table->jrt_hash[i], jrr_hash, next) {
			LIST_REMOVE(record, jrr_hash);
			free(record, M_EXT2REVOKE);
		}
	}

	free(table, M_EXT2REVOKE);
}

static int
ext2_journal_revoke_table_add(struct ext2fs_journal_revoke_table *table,
    uint32_t blocknr, uint32_t sequence)
{
	struct ext2fs_journal_revoke_record *record;
	int hash_idx;

	hash_idx = ext2_journal_revoke_hash(blocknr);

	LIST_FOREACH(record, &table->jrt_hash[hash_idx], jrr_hash) {
		if (record->jrr_blocknr == blocknr) {
			return (0);
		}
	}

	/* Create new record */
	record = malloc(sizeof(struct ext2fs_journal_revoke_record), M_EXT2REVOKE,
	    M_WAITOK | M_ZERO);
	record->jrr_blocknr = blocknr;
	record->jrr_sequence = sequence;

	LIST_INSERT_HEAD(&table->jrt_hash[hash_idx], record, jrr_hash);
	table->rt_record_count++;

	return (0);
}

static void
ext2_journal_revoke_table_clear(struct ext2fs_journal_revoke_table *table)
{
	struct ext2fs_journal_revoke_record *record, *next;
	int i;

	if (table == NULL)
		return;

	for (i = 0; i < EXT2_REVOKE_TABLE_SIZE; i++) {
		LIST_FOREACH_SAFE(record, &table->jrt_hash[i], jrr_hash, next) {
			LIST_REMOVE(record, jrr_hash);
			free(record, M_EXT2REVOKE);
		}
	}
	table->rt_record_count = 0;
}

static void
ext2_journal_revoke_list_clear(struct ext2fs_journal_revoke_list *list)
{
	struct ext2fs_journal_revoke_entry *entry, *next;

	TAILQ_FOREACH_SAFE(entry, list, jre_list, next) {
		TAILQ_REMOVE(list, entry, jre_list);
		free(entry, M_EXT2REVOKE);
	}
}

static inline int
ext2_journal_revoke_list_count(struct ext2fs_journal_revoke_list *list)
{
	struct ext2fs_journal_revoke_entry *entry;
	int count = 0;

	TAILQ_FOREACH(entry, list, jre_list) {
		count++;
	}

	return (count);
}

static int
ext2_journal_process_revoke_block(struct ext2fs_journal *jrnp, void *data,
    uint32_t sequence)
{
	struct ext2fs_journal_revoke_header *header;
	uint32_t *revoke_data;
	uint32_t revoke_count;
	uint32_t revoke_size;
	uint32_t blocknr;
	uint32_t blocksize;
	int i, error = 0;

	blocksize = jrnp->jrn_blocksize;
	header = (struct ext2fs_journal_revoke_header *)data;
	revoke_size = be32toh(header->jrh_size);

	/* Validate revoke block size */
	if (revoke_size > blocksize ||
	    revoke_size < sizeof(struct ext2fs_journal_revoke_header)) {
		return (EINVAL);
	}

	/* Calculate number of revoked blocks */
	revoke_count = (revoke_size -
			   sizeof(struct ext2fs_journal_revoke_header)) /
	    4;
	revoke_data = (uint32_t *)((char *)data +
	    sizeof(struct ext2fs_journal_revoke_header));

	EXT2_JPRINTF("Parsing revoke block: seq=%u, count=%u\n", sequence,
	    revoke_count);

	/* Add each revoked block to the table */
	for (i = 0; i < revoke_count; i++) {
		blocknr = be32toh(revoke_data[i]);
		error = ext2_journal_revoke_table_add(jrnp->jrn_revoke_table, blocknr,
		    sequence);
		if (error) {
			break;
		}
	}

	return (error);
}

static int
ext2_journal_write_revoke_block(struct ext2fs_journal *jrnp,
    struct ext2fs_journal_revoke_list *revoke_list, uint32_t *blknu)
{
	struct ext2fs_journal_revoke_header *header;
	struct ext2fs_journal_revoke_entry *entry;
	struct buf *revoke_buf;
	uint32_t *revoke_data;
	uint32_t revoke_count;
	uint32_t data_size;
	int i = 0, error;

	revoke_count = ext2_journal_revoke_list_count(revoke_list);
	if (revoke_count == 0) {
		return (0);
	}

	/* Get buffer for revoke block */
	revoke_buf = getblk(jrnp->jrn_vp, *blknu, jrnp->jrn_blocksize, 0, 0, 0);
	if (revoke_buf == NULL) {
		return (ENOMEM);
	}

	memset(revoke_buf->b_data, 0, jrnp->jrn_blocksize);

	/* Set up revoke header */
	header = (struct ext2fs_journal_revoke_header *)revoke_buf->b_data;
	header->jrh_header.jbh_magic = htobe32(EXT2_JOURNAL_MAGIC);
	header->jrh_header.jbh_blocktype = htobe32(EXT2_JOURNAL_REVOKE_BLOCK);
	header->jrh_header.jbh_sequence_num = htobe32(jrnp->jrn_sequence);

	/* Calculate size of revoke data */
	data_size = sizeof(struct ext2fs_journal_revoke_header) +
	    (revoke_count * 4);
	header->jrh_size = htobe32(data_size);

	/* Write revoked block numbers */
	revoke_data = (uint32_t *)((char *)revoke_buf->b_data +
	    sizeof(struct ext2fs_journal_revoke_header));

	TAILQ_FOREACH(entry, revoke_list, jre_list) {
		revoke_data[i] = htobe32(entry->jre_blocknr);
		i++;
	}

	error = bwrite(revoke_buf);
	if (error) {
		return (error);
	}

	*blknu = ext2_journal_next_block(jrnp, *blknu);

	EXT2_JTRACE_EXIT(0);
	return (0);
}

int
ext2_journal_revoke_block(struct ext2fs_journal *jrnp, uint32_t blocknu)
{
	struct ext2fs_journal_transaction *trans;
	struct ext2fs_journal_buf *jbuf;
	struct ext2fs_journal_revoke_entry *entry;
	int error = 0;

	if (!EXT2_JPRESENT(jrnp)) {
		return (0);
	}
	EXT2_JLOCK(jrnp);
	trans = jrnp->jrn_active_trans;

	if (trans == NULL) {
		EXT2_JUNLOCK(jrnp);
		return (EINVAL);
	}

	/* Check if already revoked */
	/* I believe the ordering of the revoke blocks would matter in full
	 * journaling mode but I don't think it matters for ordered mode */
	TAILQ_FOREACH(entry, &trans->jt_revoke_list, jre_list) {
		if (entry->jre_blocknr == blocknu) {
			EXT2_JUNLOCK(jrnp);
			return (0);
		}
	}

	entry = malloc(sizeof(*entry), M_EXT2REVOKE, M_WAITOK | M_ZERO);
	entry->jre_blocknr = blocknu;
	TAILQ_INSERT_TAIL(&trans->jt_revoke_list, entry, jre_list);

	/* check if this block is a journaled metadata block */
	jbuf = ext2_journal_find_jbuf(trans, blocknu);
	if (jbuf) {
		jbuf->jb_revoked = true;
		jbuf->jb_revoke_entry = entry;
	}

	EXT2_JUNLOCK(jrnp);
	return (error);
}

static void
ext2_journal_cancel_revoke(struct ext2fs_journal_transaction *trans,
    struct ext2fs_journal_buf *jbuf)
{
	struct ext2fs_journal_revoke_entry *entry;

	entry = jbuf->jb_revoke_entry;
	if (entry == NULL)
		return;

	TAILQ_REMOVE(&trans->jt_revoke_list, entry, jre_list);
	free(entry, M_EXT2REVOKE);

	jbuf->jb_revoked = false;
	jbuf->jb_revoke_sequence = 0;
	jbuf->jb_revoke_entry = NULL;
}

static bool ext2_journal_is_block_revoked(struct ext2fs_journal_revoke_table *table,
    uint32_t blocknr, uint32_t sequence)
{
	struct ext2fs_journal_revoke_record *record;
	int hash_idx;

	if (table == NULL || table->rt_record_count == 0) {
		return (false);
	}

	hash_idx = ext2_journal_revoke_hash(blocknr);
	LIST_FOREACH(record, &table->jrt_hash[hash_idx], jrr_hash) {
		if (record->jrr_blocknr == blocknr) {
			/*
			 * The block is considered revoked if the revoke
			 * record's sequence number is greater than or equal to
			 * the sequence of the transaction we are considering
			 * for replay.
			 */
			if (record->jrr_sequence >= sequence) {
				return (true);
			}
		}
	}

	return (false);
}

/*
 * Use the mount lock for orphan functions since
 * the fs superblock needs to be changed.
 */
int
ext2_journal_in_orphan_list(struct vnode *vp)
{
	struct inode *ip = VTOI(vp);
	struct ext2mount *ump = ip->i_ump;
	struct inode *orphan_iter;
	int found = 0;

	EXT2_LOCK(ump);
	TAILQ_FOREACH(orphan_iter, &ump->um_orphan_list, i_orphan_list) {
		if (orphan_iter == ip) {
			found = 1;
			break;
		}
	}
	EXT2_UNLOCK(ump);
	return (found);
}

/*
 * Add inode to the linked list of orphan inodes.
 *
 * The inode deletion time is used to link to the next inode
 * in the list.
 *
 * Journaling should be active when called.
 */
int
ext2_journal_add_orphan(struct vnode *vp)
{
	struct inode *ip = VTOI(vp);
	struct ext2mount *ump = ip->i_ump;
	struct m_ext2fs *fs = ump->um_e2fs;
	uint32_t old_head_inum;
	int error;

	EXT2_LOCK(ump);
	old_head_inum = fs->e2fs->e3fs_last_orphan;
	ip->i_dtime = old_head_inum;
	ip->i_flag |= IN_CHANGE;
	error = ext2_update(vp, 1);
	/* TODO better error handling. */
	if (error)
		EXT2_JERROR("inode update on orphan\n");
	fs->e2fs->e3fs_last_orphan = ip->i_number;
	fs->e2fs_fmod = 1;
	error = ext2_sbupdate(ump, 1);
	if (error)
		EXT2_JERROR("sb update on orphan\n");
	TAILQ_INSERT_HEAD(&ump->um_orphan_list, ip, i_orphan_list);
	EXT2_UNLOCK(ump);
	return (error);
}

/*
 * Deletes an inode from the orphan list.
 *
 * This function performs two operations:
 * 1. Updates the on-disk, singly-linked list
 * 2. Removes the inode from the in-memory
 * doubly-linked orphan list.
 */
int
ext2_journal_del_orphan(struct vnode *vp)
{
	struct inode *cur_ip = VTOI(vp);
	struct ext2mount *ump = cur_ip->i_ump;
	struct m_ext2fs *fs = ump->um_e2fs;
	struct inode *prev_ip, *next_ip;
	struct timespec ts;
	bool updatesb = false;
	int error = 0;

	EXT2_LOCK(ump);
	prev_ip = TAILQ_PREV(cur_ip, orphan_list_head, i_orphan_list);
	next_ip = TAILQ_NEXT(cur_ip, i_orphan_list);
	if (prev_ip != NULL) {
		prev_ip->i_dtime = (next_ip) ? next_ip->i_number : 0;
	} else {
		/* Removing head orphan inode. */
		fs->e2fs->e3fs_last_orphan = (next_ip) ? next_ip->i_number : 0;
		fs->e2fs_fmod = 1;
		updatesb = true;
	}
	/*
	 * The actual deletion time. Setting this to be non-zero
	 * should be fine since the prev inode in the list no
	 * longer points to it.
	 */
	cur_ip->i_dtime = ts.tv_sec;
	/*
	 * The order in which we update these should not matter
	 * since they should be journaled as one atomic trans.
	 */
	if (prev_ip != NULL)
		ext2_update(prev_ip->i_vnode, 1);
	ext2_update(cur_ip->i_vnode, 1);
	if (updatesb)
		ext2_sbupdate(ump, 1);

	TAILQ_REMOVE(&ump->um_orphan_list, cur_ip, i_orphan_list);
	EXT2_JPRINTF("orphan inode removed: %u", (uint32_t) cur_ip->i_number);
	EXT2_UNLOCK(ump);
	return (error);
}

static int
ext2_recover_orphan_list(struct ext2fs_journal *jrnp)
{
	struct ext2mount *ump = jrnp->jrn_em;
	struct m_ext2fs *fs = ump->um_e2fs;
	struct vnode *vp;
	struct inode *ip;
	uint32_t inum;
	uint32_t next_inum;
	int error = 0;
	inum = fs->e2fs->e3fs_last_orphan;
	if (inum != 0) {
		EXT2_JPRINTF("Starting orphan inode recovery.\n");
	} else {
		return (0);
	}

	/*
	 * FIXME: after truncate each inode, I should update the
	 * superblock's orphan pointer.
	 */
	while (inum != 0) {
		EXT2_JPRINTF("Recovering orphan inode %u.\n", inum);
		/* Get the vnode for the inode number. */
		error = VFS_VGET(ump->um_mountp, inum, LK_EXCLUSIVE, &vp);
		if (error) {
			/* Is this the right thing to do? Clear the orphan list?
			 */
			fs->e2fs->e3fs_last_orphan = 0;
			fs->e2fs_fmod = 1;
			ext2_sbupdate(ump, 1);
			return (error);
		}
		ip = VTOI(vp);
		/*
		 * Get the next orphan's inode number from the dtime field
		 * before it's cleared during truncation.
		 */
		next_inum = ip->i_dtime;
		if (ip->i_nlink > 0) {
			ip->i_nlink = 0;
			ip->i_flag |= IN_CHANGE;
		}

		/*
		 * ext2_inactive will journal the orphan recovery and it
		 * expects inode to be in in-mem orphan list.
		 */
		TAILQ_INSERT_HEAD(&ump->um_orphan_list, ip, i_orphan_list);
		VOP_INACTIVE(vp);
		inum = next_inum;
	}

	/* Complete orphan recovery. */
	error = ext2_journal_start(jrnp, 1);
	if (error)
		EXT2_JERROR("failed to journal sb orphan completion.\n");
	if (fs->e2fs->e3fs_last_orphan != 0) {
		EXT2_JPRINTF(
		    "Orphan list recovery complete. Updating superblock.\n");
		fs->e2fs->e3fs_last_orphan = 0;
		fs->e2fs_fmod = 1;
		error = ext2_sbupdate(ump, 1);
		if (error)
			EXT2_JERROR(
			    "failed to journal sb orphan completion 2.\n");
	}
	error = ext2_journal_stop(jrnp);
	return (error);
}
