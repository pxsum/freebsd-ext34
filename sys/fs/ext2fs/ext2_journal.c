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

	bqrelse(jrn_buf);
	VOP_UNLOCK(*vpp);

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static uint32_t
ext2_journal_block_type(void *data) {
	struct ext2fs_journal_block_header *jrn_bhr =
		(struct ext2fs_journal_block_header *) data;
	return (be32toh(jrn_bhr->jbh_blocktype));
}

static uint32_t
ext2_journal_tag_size(struct ext2fs_journal_sb *jsbp)
{
	uint32_t size = 0;

	/* Add checksum size if checksum v2 feature is enabled */
	if (jsbp->jsb_feature_incompat & EXT2_JOURNAL_INCOMPAT_CHECKSUM_V2) {
	    size += sizeof(uint16_t);
	}

	/* Add appropriate tag size based on 64-bit feature */
	if (jsbp->jsb_feature_incompat & EXT2_JOURNAL_INCOMPAT_64BIT) {
	    size += 16;  // 64-bit tag size includes high block number
	} else {
	    size += 12;  // 32-bit tag size
	}

	return size;
}

static int
ext2_journal_parse_desc_blk(void *data, uint32_t blk_size,
    struct ext2fs_journal *jrnp)
{
	char *c_data = (char *)data;
	int data_index = 0;
	int tag_count = 0;
	int max_size = blk_size;
	bool found_last_tag = false;
	struct ext2fs_journal_sb *jsb = jrnp->jrn_sb;
	struct ext2fs_journal_block_header *header =
		(struct ext2fs_journal_block_header *) data;
	uint32_t stride = ext2_journal_tag_size(jsb);
	struct ext2fs_journal_desc_tag *tag;

	EXT2_JTRACE_ENTER();

	EXT2_JPRINTF("desc block seq num: %u\n", be32toh(header->jbh_sequence_num));

	max_size = blk_size - sizeof(struct ext2fs_journal_block_header);
	/* Account for potential descriptor tail in checksum v2 */
	if (jsb->jsb_feature_incompat & EXT2_JOURNAL_INCOMPAT_CHECKSUM_V2) {
		max_size -= sizeof(struct ext2fs_journal_desc_tail);
	}

	/* Skip past the block header */
	c_data += sizeof(struct ext2fs_journal_block_header);
	data_index = 0;
	while (data_index + stride <= max_size) {
		tag = (struct ext2fs_journal_desc_tag *)(&(c_data[data_index]));

		// TODO only print when debug flag is on
		uint16_t flags = be16toh(tag->jdt_flags);
		uint16_t checksum = be16toh(tag->jdt_checksum);
		uint32_t blocknum_low = be32toh(tag->jdt_blocknum_low);
		uint32_t blocknum_high = be32toh(tag->jdt_blocknum_high);

		printf("desc blk: tag num: %d\n", tag_count);
		printf("desc blk: tag flag: %u\n", flags);
		printf("desc blk: tag checksum: %u\n", checksum);
		printf("desc blk: blocknum low: %u\n", blocknum_low);
		printf("desc blk: blocknum high: %u\n", blocknum_high);

		if (flags & EXT2_JOURNAL_TAG_LAST_ENTRY) {
			found_last_tag = true;
			tag_count++;
			break;
		}

		/* Move to next tag position */
		data_index += stride;

		if (!(flags & EXT2_JOURNAL_TAG_SAME_UUID)) {
			data_index += 16; // UUID is 16 bytes
			/* Additional bounds check after UUID */
			if (data_index >= max_size) {
				EXT2_JPRINTF("UUID field extends beyond block boundary\n");
				break;
			}
		}
		tag_count++;
	}

	if (!found_last_tag) {
		EXT2_JERROR("reached the end of block without last entry flag\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	EXT2_JTRACE_EXIT(tag_count);
	return tag_count;
}

// TODO check seq number of commits to ensure valid commit
static int
ext2_journal_walk_trans(struct ext2fs_journal *jrnp, uint32_t trans_start,
	uint32_t *next_trans_start)
{
	int error, blk_count;
	struct buf *jrn_buf;
	void *jrn_data;
	uint32_t curr_blk;
	struct vnode *vp = jrnp->jrn_vp;
	struct m_ext2fs *fs = jrnp->jrn_fs;

	EXT2_JTRACE_ENTER();

	/* Read the descriptor block */
	error = bread(vp, trans_start, (daddr_t) fs->e2fs_bsize, NOCRED, &jrn_buf);
	if (error) {
		EXT2_JERROR("desc block read fail: %d\n", error);
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	jrn_data = jrn_buf->b_data;
	if (!ext2_journal_verify_block(jrn_data)) {
		EXT2_JERROR("desc block has invalid magic num\n");
		brelse(jrn_buf);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}
	if (ext2_journal_block_type(jrn_data) != EXT2_JOURNAL_DESCRIPTOR_BLOCK) {
		EXT2_JERROR("no valid des block\n");
		brelse(jrn_buf);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}
	/* Read and print descriptor block data */
	blk_count = ext2_journal_parse_desc_blk(jrn_buf, jrnp->jrn_blocksize, jrnp);
	brelse(jrn_buf);

	if (blk_count < 0) {
		EXT2_JERROR("invalid blk count for trans/n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	EXT2_JPRINTF("transaction has %d data blocks\n", blk_count);

	/* Read and print data in actual journal */
	curr_blk = trans_start + 1;
	for (int i = 0; i < blk_count; i++) {
		/* Handle journal wraparound */
		if (curr_blk > jrnp->jrn_last) {
			curr_blk = jrnp->jrn_first + (curr_blk - jrnp->jrn_last);
		}

		error = bread(vp, curr_blk, (daddr_t) fs->e2fs_bsize, NOCRED, &jrn_buf);
		if (error) {
			EXT2_JERROR("data block %u read fail: %d/n", curr_blk, error);
			EXT2_JTRACE_EXIT(error);
			return (error);
		}

		jrn_data = jrn_buf->b_data;

		if (ext2_journal_verify_block(jrn_data)) {
			uint32_t block_type = ext2_journal_block_type(jrn_data);
			EXT2_JPRINTF(" WARNING: data block has journal magic type=%u/n", block_type);
		}
		brelse(jrn_buf);
		curr_blk++;
	}

	/* We expect the next block to be a commit or revoke block */
	if (curr_blk > jrnp->jrn_last) {
		curr_blk = jrnp->jrn_first + (curr_blk - jrnp->jrn_last);
	}

	error = bread(vp, curr_blk, (daddr_t) fs->e2fs_bsize, NOCRED, &jrn_buf);
	if (error) {
		EXT2_JERROR("commit/revoke block read fail: %d\n", error);
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	jrn_data = jrn_buf->b_data;
	if (!ext2_journal_verify_block(jrn_data)) {
		EXT2_JERROR("commit/revoke block invalid magic\n");
		brelse(jrn_buf);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	uint32_t block_type = ext2_journal_block_type(jrn_data);
	struct ext2fs_journal_block_header *header =
		(struct ext2fs_journal_block_header *) jrn_data;

	if (block_type == EXT2_JOURNAL_COMMIT_BLOCK) {
		EXT2_JPRINTF("found commit block at %u\n", curr_blk);
		EXT2_JPRINTF("commit seq num: %u\n", be32toh(header->jbh_sequence_num));
	} else if (block_type == EXT2_JOURNAL_REVOKE_BLOCK) {
		EXT2_JPRINTF("found revoke block at %u\n", curr_blk);
		EXT2_JPRINTF("revoke seq num: %u\n", be32toh(header->jbh_sequence_num));
		// TODO parse revoke block
		// TODO track a list of global reboke blocks to ensure we do not
		// replay those blocks
	} else {
		EXT2_JERROR("unexpected block type %u at %u\n",
		    block_type, curr_blk);
		brelse(jrn_buf);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	brelse(jrn_buf);

	/* Calculate start of next transaction */
	curr_blk++;
	if (curr_blk > jrnp->jrn_last) {
		curr_blk = jrnp->jrn_first + (curr_blk - jrnp->jrn_last);
	}

	EXT2_JPRINTF("next transaction should start at %u\n", curr_blk);
	*next_trans_start = curr_blk;

	EXT2_JTRACE_EXIT(0);
	return (0);
}

/*
 * Starts journal recovery / replay.
 */
int
ext2_journal_recover(struct ext2fs_journal *jrnp)
{
	uint32_t curr_trans_start = jrnp->jrn_log_start;
	int32_t next_trans_start;
	int error;

	EXT2_JTRACE_ENTER();

	if (!(jrnp->jrn_flags & EXT2_JOURNAL_NEEDS_RECOVERY)) {
		EXT2_JERROR("recovery not needed\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	/* Parse transactions for now */
	/* While valid transaction, parse the next transaction */
	while ((error =
		ext2_journal_walk_trans(jrnp, curr_trans_start, &next_trans_start)) == 0) {
		if (next_trans_start == jrnp->jrn_log_start) {
			EXT2_JPRINTF("reached starting point\n");
			break;
		}

		if (next_trans_start == curr_trans_start) {
			EXT2_JPRINTF("no progress made\n");
			break;
		}

		curr_trans_start = next_trans_start;
	}

	/* Assume parsing error means end of our journal or corruption. */
	if (error != 0) {
		EXT2_JERROR("parsing error at: %d\n", next_trans_start);
		return (error);
	}

	EXT2_JTRACE_EXIT(0);
	return (0);
}

/*
 * Initialize the in-memory journal structure.
 *
 * Populate the ext2fs_journal structurre with useful parameters from the
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

	jrnp->jrn_active_trans = NULL;
	jrnp->jrn_committing_trans = NULL;
	TAILQ_INIT(&jrnp->jrn_checkpoint_list);

	jrnp->jrn_blocksize = be32toh(disk_sb->jsb_blocksize);
	jrnp->jrn_max_blocks = be32toh(disk_sb->jsb_max_blocks);
	jrnp->jrn_first = be32toh(disk_sb->jsb_first_block);
	jrnp->jrn_last = jrnp->jrn_first + jrnp->jrn_max_blocks - 1;
	jrnp->jrn_free_blocks = jrnp->jrn_max_blocks; /* need to adjust */
	jrnp->jrn_log_end = jrnp->jrn_log_start; /* TODO update later during recover */

	if (jrnp->jrn_max_blocks < EXT2_JOURNAL_MIN_BLOCKS) {
		EXT2_JERROR("journal number of blocks too little\n");
		mtx_destroy(&jrnp->jrn_lock);
		cv_destroy(&jrnp->jrn_trans_commit_cv);
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
	if (jrnp == NULL)
		return (0);

	if (jrnp->jrn_vp != NULL)
		vput(jrnp->jrn_vp);

	if (jrnp->jrn_sb != NULL)
		free(jrnp->jrn_sb, M_EXT2JSB);

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
	int error;
	struct ext2mount *ump = VFSTOEXT2(mp);
	struct m_ext2fs *fs = ump->um_e2fs;

	EXT2_JTRACE_ENTER();

	*jrnpp = malloc(sizeof(struct ext2fs_journal), M_EXT2JOURNAL,
	    M_WAITOK | M_ZERO);

	error = ext2_journal_open_inode(mp, &((*jrnpp)->jrn_vp),
                              &((*jrnpp)->jrn_sb));
	if (error != 0) {
		EXT2_JERROR("failed to open journal inode. error: %d\n", error);
		ext2_journal_close(*jrnpp);
		*jrnpp = NULL;
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	(*jrnpp)->jrn_fs = fs;
	error = ext2_journal_init(*jrnpp);
	if (error != 0) {
		EXT2_JERROR("failed initialize journal. error: %d\n", error);
		ext2_journal_close(*jrnpp);
		*jrnpp = NULL;
		EXT2_JTRACE_EXIT(error);
		return (error);
	}
	ump->um_journal = *jrnpp;

	EXT2_JTRACE_EXIT(0);
	return (0);
}


/*
 * Buffer i/o completion callback to ensure data blocks written before metadata
 * in ordered-mode.
 */
static void
ext2_journal_biodone(struct buf *bp)
{
	struct ext2_journal_buf *jbuf;
	struct ext2fs_journal_transaction *trans;
	struct ext2fs_journal *jrnp;

	EXT2_JTRACE_ENTER();

	jbuf = bp->b_fsprivate1;
	trans = jbuf->jb_owning_trans;
	jrnp = trans->jt_journal;

	mtx_lock(&jrnp->jrn_lock);
	trans->jt_pending_data--;

	if (trans->jt_pending_data == 0) {
		/* All data I/O complete so signal waiters */
		cv_broadcast(&trans->jt_iowait_cv);
	}

	mtx_unlock(&jrnp->jrn_lock);
	EXT2_JTRACE_EXIT(0);
}

/*
 * Allocate and initialize a journal buffer.
 */
static struct ext2_journal_buf *
ext2_journal_buf_alloc(struct ext2fs_journal *jrnp, struct buf *bp,
    enum ext2_journal_buf_type type)
{
	struct ext2_journal_buf *jbuf;

	EXT2_JTRACE_ENTER();

	jbuf = malloc(sizeof(struct ext2_journal_buf), M_EXT2JBUF,
	    M_WAITOK | M_ZERO);

	jbuf->jb_owning_trans = jrnp->jrn_active_trans;
	jbuf->jb_buf = bp;
	jbuf->jb_type = type;
	jbuf->jb_blocknr = bp->b_lblkno;

	/* Faster way to find the jbuf */
	bp->b_fsprivate1 = jbuf;

	EXT2_JTRACE_EXIT(0);
	return (jbuf);
}

/*
 * Free a journal buf type.
 */
static void
ext2_journal_buf_free(struct ext2_journal_buf *jbuf)
{
	KASSERT(jbuf != NULL, "jbuf to free is NULL\n");
	KASSERT(jbuf->jb_buf == NULL, "buf of jbuf is NOT NULL\n");
	EXT2_JTRACE_ENTER();

	free(jbuf, M_EXT2JBUF);
}

/*
 * Free all jbufs in a list.
 */
static void
ext2_journal_buf_free_list(struct ext2_journal_buf_list *head)
{
	struct ext2_journal_buf *jbuf, *next;

	EXT2_JTRACE_ENTER();

	// TODO maybe i should free buf here as well

	TAILQ_FOREACH_SAFE(jbuf, head, jb_list, next) {
		TAILQ_REMOVE(head, jbuf, jb_list);
		ext2_journal_buf_free(jbuf);
	}
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
	    M_WAITOK | M_ZERO);

	trans->jt_journal = journal;
	trans->jt_state = EXT2_TRANS_RUNNING;
	trans->jt_refcount = 0;
	trans->jt_owner = NULL;
	trans->jt_blocks_used = 0;
	trans->jt_blocks_reserved = 0;
	trans->jt_data_count = 0;
	trans->jt_metadata_count = 0;
	trans->jt_pending_data = 0;

	TAILQ_INIT(&trans->jt_data_buffers);
	TAILQ_INIT(&trans->jt_metadata_buffers);
	cv_init(&trans->jt_iowait_cv, "jrniowait");

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

	/* issue if still waiting for data writes */
	KASSERT(trans->jt_pending_data == 0,
	    ("ext2_journal_transaction_free: pending I/O"));

	/* Free all journal buffer descriptors */
	ext2_journal_buf_free_list(&trans->jt_data_buffers);
	ext2_journal_buf_free_list(&trans->jt_metadata_buffers);

	/* Destroy condition variable */
	cv_destroy(&trans->jt_iowait_cv);

	free(trans, M_EXT2JTRANS);
}

/*
 * Mark a data jbuf as dirty.
 */
int
ext2_journal_dirty_data(struct ext2fs_journal *jrnp, struct buf *bp)
{
	struct ext2fs_journal_transaction *trans;
	struct ext2_journal_buf *jbuf;

	EXT2_JTRACE_ENTER();

	mtx_lock(&jrnp->jrn_lock);
	trans = jrnp->jrn_active_trans;

	if (trans == NULL || trans->jt_owner != curthread) {
		mtx_unlock(&jrnp->jrn_lock);
		return (EINVAL);
	}

	/* Check if buffer already tracked */
	TAILQ_FOREACH(jbuf, &trans->jt_data_buffers, jb_list) {
		if (jbuf->jb_buf == bp) {
			mtx_unlock(&jrnp->jrn_lock);
			EXT2_JPRINTF("jbuf data found\n");
			EXT2_JTRACE_EXIT(0);
			return (0);
		}
	}

	jbuf = ext2_journal_buf_alloc(jrnp, bp, EXT2_JBUF_DATA);

	/* Set up completion callback */
	bp->b_fsprivate1 = jbuf;
	if (bp->b_iodone != NULL) {
		mtx_unlock(&jrnp->jrn_lock);
		EXT2_JERROR("assumption of b_iodone being not used is wrong\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}
	bp->b_iodone = ext2_journal_biodone;

	TAILQ_INSERT_TAIL(&trans->jt_data_buffers, jbuf, jb_list);
	trans->jt_data_count++;
	trans->jt_pending_data++;

	mtx_unlock(&jrnp->jrn_lock);
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
	struct ext2_journal_buf *jbuf;

	EXT2_JTRACE_ENTER();

	mtx_lock(&jrnp->jrn_lock);
	trans = jrnp->jrn_active_trans;

	if (trans == NULL || trans->jt_owner != curthread) {
		EXT2_JPRINTF("trans null or not current thread\n");
		mtx_unlock(&jrnp->jrn_lock);
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	/* Check if buffer already tracked */
	TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
		KASSERT(jbuf->jb_buf != NULL, "NULL jbuf buf ref");
		if (jbuf->jb_buf == bp) {
			EXT2_JPRINTF("jbuf metadata found\n");
			EXT2_JPRINT_JBUF(jbuf);
			mtx_unlock(&jrnp->jrn_lock);
			EXT2_JTRACE_EXIT(0);
			return (0);
		}
	}


	KASSERT(bp->b_iodone != NULL, "assumption of b_iodone not used is wrong\n");

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

	jbuf = ext2_journal_buf_alloc(jrnp, bp, EXT2_JBUF_METADATA);

	EXT2_JPRINTF("new jbuf created\n");

	/* Notifies the buffer cache we are doing our own management */
	bp->b_flags |= B_MANAGED;

	TAILQ_INSERT_TAIL(&trans->jt_metadata_buffers, jbuf, jb_list);
	trans->jt_metadata_count++;
	EXT2_JPRINTF("new jbuf added to metadata list\n");

	/* Unlocks the buf */
	bqrelse(bp);

	mtx_unlock(&jrnp->jrn_lock);
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

	EXT2_JTRACE_ENTER();

	mtx_lock(&jrnp->jrn_lock);
	while ((trans = jrnp->jrn_active_trans) != NULL) {
		/* Same thread so must be a nested file operation */
		if (trans->jt_owner == td) {
			trans->jt_refcount++;
			trans->jt_blocks_reserved += nblocks;
			mtx_unlock(&jrnp->jrn_lock);
			EXT2_JPRINTF("journal start joined nested operation\n");
			EXT2_JTRACE_EXIT(0);
			return (0);
		}

		/* Wait to start a new trans */
		cv_wait(&jrnp->jrn_trans_start_cv, &jrnp->jrn_lock);
	}

	/*
	 * We wait for the commit to finish since a new transaction might need
	 * to write to a block that is part of the committing transaction. If
	 * we naively write to a committing block, we can corrupt the
	 * transaction. We can do COW to not wait but just wait for now.
	 */
	while (jrnp->jrn_committing_trans != NULL) {
		cv_wait(&jrnp->jrn_trans_commit_cv, &jrnp->jrn_lock);
	}

	/* Start new transaction */
	trans = ext2_journal_transaction_alloc(jrnp);
	trans->jt_journal = jrnp;
	trans->jt_state = EXT2_TRANS_RUNNING;
	trans->jt_refcount = 1;
	trans->jt_owner = td;
	trans->jt_blocks_reserved = nblocks;
	trans->jt_pending_data = 0;

	EXT2_JPRINTF("new transaction started\n");

	jrnp->jrn_active_trans = trans;
	mtx_unlock(&jrnp->jrn_lock);

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_write_desc_blk(struct ext2fs_journal *jrnp, uint32_t *blknu)
{
	struct ext2fs_journal_transaction *trans = jrnp->jrn_committing_trans;
	struct ext2fs_journal_block_header *header;
	struct ext2fs_journal_desc_tag *tag;
	struct ext2_journal_buf *jbuf;
	struct buf *desc_buf;
	char *desc_data;
	uint32_t tag_offset = sizeof(struct ext2fs_journal_block_header);
	uint32_t tag_size = ext2_journal_tag_size(jrnp->jrn_sb);
	int error;
	bool last_tag = false;


	EXT2_JTRACE_ENTER();

	desc_buf = getblk(jrnp->jrn_vp, *blknu, jrnp->jrn_blocksize, 0, 0, 0);
	if (desc_buf == NULL) {
		EXT2_JERROR("ext2_journal_write_desc_blk: getblk failed\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	desc_data = desc_buf->b_data;
	memset(desc_data, 0, jrnp->jrn_blocksize);

	header = (struct ext2fs_journal_block_header *) desc_data;
	header->jbh_magic = htobe32(EXT2_JOURNAL_MAGIC);
	header->jbh_blocktype = htobe32(EXT2_JOURNAL_DESCRIPTOR_BLOCK);
	header->jbh_sequence_num = htobe32(jrnp->jrn_sequence);

	/* Write tags for each metadata buf */
	TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
		// TODO ensure metadata fits in one desc blk, or split into mult
		tag = (struct ext2fs_journal_desc_tag *)(desc_data +
		    tag_offset);

		/* Check if this is the last tag */
		last_tag = (TAILQ_NEXT(jbuf, jb_list) == NULL);

		tag->jdt_blocknum_low = htobe32(jbuf->jb_blocknr);
		tag->jdt_flags = htobe16(
		    last_tag ? EXT2_JOURNAL_TAG_LAST_ENTRY : 0);
		tag->jdt_checksum = 0; /* TODO: implement checksums */

		tag_offset += tag_size;

		if (last_tag)
			break;
	}
	/* Write desc block syncly for now */
	error = bwrite(desc_buf);
	if (error) {
		EXT2_JERROR("bwrite fail\n");
		EXT2_JTRACE_EXIT(error);
		brelse(desc_buf);
		return (error);
	}


	/* Handle circular journal wraparound */
	(*blknu)++;
	if (*blknu > jrnp->jrn_last) {
		*blknu = jrnp->jrn_first;
	}

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_write_commit_blk(struct ext2fs_journal *jrnp,
    uint32_t *blknu)
{
	struct ext2fs_journal_commit_header *header;
	struct buf *commit_buf;
	int error;

	EXT2_JTRACE_ENTER();

	commit_buf = getblk(jrnp->jrn_vp, *blknu, jrnp->jrn_blocksize,
	    0, 0, 0);
	if (commit_buf == NULL) {
		EXT2_JERROR("getblk failed\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	// check if getblk memsets for us
	memset(commit_buf->b_data, 0, jrnp->jrn_blocksize);

	header = (struct ext2fs_journal_commit_header *) commit_buf->b_data;
	header->jch_header.jbh_magic = htobe32(EXT2_JOURNAL_MAGIC);
	header->jch_header.jbh_blocktype = htobe32(EXT2_JOURNAL_COMMIT_BLOCK);
	header->jch_header.jbh_sequence_num = htobe32(jrnp->jrn_sequence);

	error = bwrite(commit_buf);
	if (error) {
		EXT2_JERROR("bwrite failed: %d\n", error);
		EXT2_JTRACE_EXIT(error);
		return (error);
	}

	(*blknu)++;
	/* Handle circular journal wraparound */
	if (*blknu > jrnp->jrn_last) {
		*blknu = jrnp->jrn_first;
	}

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_checkpoint_metadata(struct ext2fs_journal *jrnp,
    struct ext2fs_journal_transaction *trans)
{
	struct ext2_journal_buf *jbuf;
	struct buf *bp;
	int error = 0;

	EXT2_JTRACE_ENTER();
	TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
		bp = jbuf->jb_buf;
		error = BUF_LOCK(bp,  LK_EXCLUSIVE | LK_NOWAIT, NULL);
		if (error) {
			EXT2_JERROR("failed to lock the buf before brelse: %d\n", error);
			return (error);
		}
		// TODO confirm buffer management and freeing here
		bp->b_flags &= ~B_MANAGED;
		error = bwrite(bp);
		if (error) {
			brelse(bp);
			jbuf->jb_buf = NULL;
			EXT2_JERROR("checkpoing write failed: %d\n", error);
			return (error);
		}

		jbuf->jb_buf = NULL;
	}

	EXT2_JTRACE_EXIT(error);
	return (error);
}

int
ext2_journal_checkpoint_trans(struct ext2fs_journal *jrnp)
{
	struct ext2fs_journal_transaction *trans, *next_trans;
	int freed_blocks = 0;
	int error = 0;

	EXT2_JTRACE_ENTER();
	mtx_lock(&jrnp->jrn_lock);

	TAILQ_FOREACH_SAFE(trans, &jrnp->jrn_checkpoint_list,
	    jt_checkpoint_entry, next_trans) {
		if (trans->jt_refcount > 0) {
			//major error;
			EXT2_JERROR("transaction is still referenced\n");
			mtx_unlock(&jrnp->jrn_lock);
			EXT2_JTRACE_EXIT(EINVAL);
			return (EINVAL);
		}

		error = ext2_journal_checkpoint_metadata(jrnp, trans);
		if (error) {
			EXT2_JERROR("checkpoint metadata failed\n");
			mtx_unlock(&jrnp->jrn_lock);
			EXT2_JTRACE_EXIT(EINVAL);
			return (EINVAL);
		}

		TAILQ_REMOVE(&jrnp->jrn_checkpoint_list, trans, jt_checkpoint_entry);
		freed_blocks += trans->jt_blocks_reserved;

		ext2_journal_transaction_free(trans);
	}

	if (freed_blocks > 0) {
		jrnp->jrn_free_blocks += freed_blocks;
		/* Update the log start */
		jrnp->jrn_log_start = jrnp->jrn_log_end;

		// TODO update superblock
	}

	mtx_unlock(&jrnp->jrn_lock);

	EXT2_JTRACE_EXIT(0);
	return (0);
}

static int
ext2_journal_commit_trans(struct ext2fs_journal *jrnp)
{
	struct ext2fs_journal_transaction *trans;
	struct ext2_journal_buf *jbuf;
	struct buf *sb_buf;
	struct buf *disk_jbuf;
	struct ext2fs_journal_sb *disk_sb;
	struct vnode *jrn_vp = jrnp->jrn_vp;
	uint32_t jrn_blknu;
	int error = 0;
	static bool first_commit = false; /* false for now */

	EXT2_JTRACE_ENTER();

	mtx_lock(&jrnp->jrn_lock);

	/* Ensure no active transaction while journaling */
	KASSERT(jrnp->jrn_active_trans != NULL,
	    "ext2_journal_commit_trans: active trans\n");

	trans = jrnp->jrn_committing_trans;

	if (trans == NULL) {
		mtx_unlock(&jrnp->jrn_lock);
		EXT2_JERROR("trans to commit is NULL\n");
		EXT2_JTRACE_EXIT(EINVAL);
		return (EINVAL);
	}

	EXT2_JPRINT_TRANS(trans);
	EXT2_JPRINT_TRANS_BUFFERS(trans);

	/* Allocate journal blocks */
	jrn_blknu = jrnp->jrn_log_end;

	/* Wait for all data i/o to finish first in ordered mode */
	while (trans->jt_pending_data > 0) {
		cv_wait(&trans->jt_iowait_cv, &jrnp->jrn_lock);
	}

	EXT2_JPRINTF("All data i/o to wait on done\n");

	trans->jt_state = EXT2_TRANS_COMMIT;

	if (trans->jt_metadata_count > 0) {
		/* Write descriptor block */
		error = ext2_journal_write_desc_blk(jrnp, &jrn_blknu);
		if (error) {
			EXT2_JPRINTF("write desc blk failed\n");
			goto cleanup;
		}

		/* Write metadata blocks to journal */
		TAILQ_FOREACH(jbuf, &trans->jt_metadata_buffers, jb_list) {
			KASSERT(jbuf->jb_buf != NULL, "NULL jbuf->jb_buf");
			/* Get journal buffer */
			disk_jbuf = getblk(jrnp->jrn_vp, jrn_blknu,
			    jrnp->jrn_blocksize, 0, 0, 0);
			if (disk_jbuf == NULL) {
				EXT2_JPRINTF("getblk failed\n");
				goto cleanup;
			}

			/* Copy metadata to on-disk journal buf */
			memcpy(disk_jbuf->b_data, jbuf->jb_buf->b_data,
			    jrnp->jrn_blocksize);

			/* Write to journal, sync for now */
			error = bwrite(disk_jbuf);
			if (error) {
				EXT2_JPRINTF("bwrite failed\n");
				brelse(disk_jbuf);
				goto cleanup;
			}

			jrn_blknu++;
			/* Handle circular journal wraparound */
			if (jrn_blknu > jrnp->jrn_last) {
				jrn_blknu = jrnp->jrn_first;
			}
		}

		/* Write commit block */
		jrnp->jrn_sequence++;
		error = ext2_journal_write_commit_blk(jrnp, &jrn_blknu);
		if (error) {
			EXT2_JPRINTF("write commit blk failed\n");
			goto cleanup;
		}

		jrnp->jrn_log_end = jrn_blknu;

		/* Update disk-superblock starting seq number*/
		// Skip for now
		if (first_commit) {
			EXT2_JPRINTF("first commit so updating seq nu\n");
			jrnp->jrn_sb->jsb_sequence_id = jrnp->jrn_sequence;
			error = bread(jrn_vp, 0, jrnp->jrn_blocksize,
			    NOCRED, &sb_buf);
			if (error) {
				EXT2_JERROR("bread failed: %d\n", error);

				// or go to cleanup
				return (error);
			}
			disk_sb = (struct ext2fs_journal_sb *) sb_buf->b_data;
			/* TODO verify this */
			disk_sb->jsb_sequence_id = htobe32(jrnp->jrn_sequence);

			error = bwrite(sb_buf);
			if (error) {
				EXT2_JERROR("bwrite failed for superblock\n");
				brelse(sb_buf);
				return (error);
			}
			first_commit = false;
		}
	}

	/* Move commited trans to checkpoing queue */
	TAILQ_INSERT_TAIL(&jrnp->jrn_checkpoint_list, trans, jt_checkpoint_entry);
	jrnp->jrn_committing_trans = NULL;

	/* Wake up thread waiting on commmit */
	cv_signal(&jrnp->jrn_trans_commit_cv);
	mtx_unlock(&jrnp->jrn_lock);

	EXT2_JTRACE_EXIT(0);
	return (0);
cleanup:
	// TODO
	EXT2_JTRACE_EXIT(0);
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
	bool should_commit = false;

	EXT2_JTRACE_ENTER();

	mtx_lock(&jrnp->jrn_lock);
	trans = jrnp->jrn_active_trans;

	if (trans == NULL || trans->jt_owner != td) {
		mtx_unlock(&jrnp->jrn_lock);
		return (EINVAL);
	}

	trans->jt_refcount--;
	if (trans->jt_refcount == 0) {
		/* Last reference so commit for now */
		/* Can optimize later */
		jrnp->jrn_active_trans = NULL;
		jrnp->jrn_committing_trans = trans;
		should_commit = true;

		/* Wake up any threads waiting to start new transaction */
		// FIXME do not start a transaction if commiting
		cv_signal(&jrnp->jrn_trans_start_cv);
	}

	mtx_unlock(&jrnp->jrn_lock);
	if (should_commit) {
		EXT2_JPRINTF("Journal comitting\n");
		// TODO maybe just hold lock while calling here
		return ext2_journal_commit_trans(jrnp);
	}

	EXT2_JTRACE_EXIT(0);
	return (0);
}

/*
 * Force a journal commit.
 */
int
ext2_journal_force_commit(struct ext2fs_journal *jrnp) {
	struct ext2fs_journal_transaction *trans;

	mtx_lock(&jrnp->jrn_lock);

	/* Wait for active transactions to finish */
	while ((trans = jrnp->jrn_active_trans) != NULL){
		cv_wait(&jrnp->jrn_trans_commit_cv, &jrnp->jrn_lock);
	}

	if ((trans = jrnp->jrn_committing_trans) != NULL) {
		mtx_unlock(&jrnp->jrn_lock);
		return ext2_journal_commit_trans(jrnp);
	}
	mtx_unlock(&jrnp->jrn_lock);
	return (0);
}
