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
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/inode.h>

#include <fs/ext2fs/ext2_extern.h>

MALLOC_DEFINE(M_EXT2JOURNAL, "ext2fs_journal", "In-memory ext2 journal");
MALLOC_DEFINE(M_EXT2JSB, "ext2fs_journal_sb", "In-memory copy of \
	journal superblock");

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
	int error;


	/* Check if journal inode number is valid */
	if (fs->e2fs->e3fs_journal_inum == 0 ||
	    fs->e2fs->e3fs_journal_inum != EXT2_JOURNALINO) {
		printf("ext2fs: invalid journal inode number: %u\n",
		    fs->e2fs->e3fs_journal_inum);
		return (EINVAL);
	}

	error = VFS_VGET(mp, EXT2_JOURNALINO, LK_EXCLUSIVE, vpp);
	if (error != 0) {
		*vpp = NULL;
		printf("ext2fs: vfs_get failed: %d\n", error);
		return (error);
	}

	/* bufobj size must be initilized else panics */
	if ((*vpp)->v_bufobj.bo_bsize == 0) {
	    (*vpp)->v_bufobj.bo_bsize = fs->e2fs_bsize;
	}

	error = bread(*vpp, 0, (daddr_t) fs->e2fs_bsize, NOCRED, &jrn_buf);
	if (error != 0) {
		printf("ext2fs: bread failed: %d\n", error);
		vput(*vpp);
		*vpp = NULL;
		return (error);
	}

	jrn_data = jrn_buf->b_data;
	if (!ext2_journal_verify_block(jrn_data)) {
		printf("ext2fs: journal magic number mismatch\n");
		brelse(jrn_buf);
		vput(*vpp);
		*vpp = NULL;
		return (EINVAL);
	}

	jrn_sbp = (struct ext2fs_journal_sb *) jrn_data;
	if (be32toh(jrn_sbp->jsb_header.jbh_blocktype) !=
	    EXT2_JOURNAL_FORMAT_BASIC &&
	    be32toh(jrn_sbp->jsb_header.jbh_blocktype) !=
	    EXT2_JOURNAL_FORMAT_EXTENDED)
	    {
		    printf("ext2fs: journal is not the proper version\n");
		    brelse(jrn_buf);
		    vput(*vpp);
		    *vpp = NULL;
		    return (EINVAL);
	    }

	*jrn_sbpp = (struct ext2fs_journal_sb *)
		malloc(sizeof(struct ext2fs_journal_sb), M_EXT2JSB, M_WAITOK);

	ext2_jsb_from_disk(*jrn_sbpp, jrn_sbp);

	brelse(jrn_buf);
	VOP_UNLOCK(*vpp);
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

	printf("desc block seq num: %u\n", be32toh(header->jbh_sequence_num));

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
				printf("desc blk: UUID field extends beyond block boundary\n");
				break;
			}
		}
		tag_count++;
	}

	if (!found_last_tag) {
		printf("desc blk: reached the end of block without last entry flag\n");
		return (EINVAL);
	}

	printf("desc blk: total tags found: %d\n", tag_count);
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

	/* Read the descriptor block */
	error = bread(vp, trans_start, (daddr_t) fs->e2fs_bsize, NOCRED, &jrn_buf);
	if (error) {
		printf("ext2_journal_walk_trans: desc block read fail: %d\n", error);
		return (error);
	}

	jrn_data = jrn_buf->b_data;
	if (!ext2_journal_verify_block(jrn_data)) {
		printf("ext2_journal_walk_trans: desc block has invalid magic num\n");
		brelse(jrn_buf);
		return (EINVAL);
	}
	if (ext2_journal_block_type(jrn_data) != EXT2_JOURNAL_DESCRIPTOR_BLOCK) {
		printf("ext2_journal_walk_trans: no valid des block\n");
		brelse(jrn_buf);
		return (EINVAL);
	}
	/* Read and print descriptor block data */
	blk_count = ext2_journal_parse_desc_blk(jrn_buf, jrnp->jrn_blocksize, jrnp);
	brelse(jrn_buf);

	if (blk_count < 0) {
		printf("ext2_journal_walk_trans: invalid blk count for trans/n");
		return (EINVAL);
	}

	printf("ext2_journal_walk_trans: transaction has %d data blocks\n", blk_count);

	/* Read and print data in actual journal */
	curr_blk = trans_start + 1;
	for (int i = 0; i < blk_count; i++) {
		/* Handle journal wraparound */
		if (curr_blk > jrnp->jrn_last) {
			curr_blk = jrnp->jrn_first + (curr_blk - jrnp->jrn_last);
		}

		error = bread(vp, curr_blk, (daddr_t) fs->e2fs_bsize, NOCRED, &jrn_buf);
		if (error) {
			printf("ext2_journal_walk_trans: data block %u read fail: %d/n", curr_blk, error);
			return (error);
		}

		jrn_data = jrn_buf->b_data;

		if (ext2_journal_verify_block(jrn_data)) {
			uint32_t block_type = ext2_journal_block_type(jrn_data);
			printf(" WARNING: data block has journal magic type=%u/n", block_type);
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
		printf("ext2_journal_walk_trans: commit/revoke block read fail: %d\n", error);
		return (error);
	}

	jrn_data = jrn_buf->b_data;
	if (!ext2_journal_verify_block(jrn_data)) {
		printf("ext2_journal_walk_trans: commit/revoke block invalid magic\n");
		brelse(jrn_buf);
		return (EINVAL);
	}

	uint32_t block_type = ext2_journal_block_type(jrn_data);
	struct ext2fs_journal_block_header *header =
		(struct ext2fs_journal_block_header *) jrn_data;

	if (block_type == EXT2_JOURNAL_COMMIT_BLOCK) {
		printf("ext2_journal_walk_trans: found commit block at %u\n", curr_blk);
		printf("  commit seq num: %u\n", be32toh(header->jbh_sequence_num));
	} else if (block_type == EXT2_JOURNAL_REVOKE_BLOCK) {
		printf("ext2_journal_walk_trans: found revoke block at %u\n", curr_blk);
		printf("  revoke seq num: %u\n", be32toh(header->jbh_sequence_num));
		// TODO parse revoke block
		// TODO track a list of global reboke blocks to ensure we do not
		// replay those blocks
	} else {
		printf("ext2_journal_walk_trans: unexpected block type %u at %u\n",
		    block_type, curr_blk);
		brelse(jrn_buf);
		return (EINVAL);
	}

	brelse(jrn_buf);

	/* Calculate start of next transaction */
	curr_blk++;
	if (curr_blk > jrnp->jrn_last) {
		curr_blk = jrnp->jrn_first + (curr_blk - jrnp->jrn_last);
	}

	printf("ext2_journal_walk_trans: next transaction should start at %u\n", curr_blk);
	*next_trans_start = curr_blk;
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

	printf("IN ext2_journal_recover\n");
	if (!(jrnp->jrn_flags & EXT2_JOURNAL_NEEDS_RECOVERY)) {
		printf("ext2_journal_recover: recovery not needed\n");
		return (EINVAL);
	}

	/* Parse transactions for now */
	/* While valid transaction, parse the next transaction */
	while ((error =
		ext2_journal_walk_trans(jrnp, curr_trans_start, &next_trans_start)) == 0) {
		if (next_trans_start == jrnp->jrn_log_start) {
			printf("ext2_journal_recover: reached starting point\n");
			break;
		}

		if (next_trans_start == curr_trans_start) {
			printf("ext2_journal_recover: no progress made\n");
			break;
		}

		curr_trans_start = next_trans_start;
	}

	/* Assume parsing error means end of our journal or corruption. */
	if (error != 0) {
		printf("ext2_journal_recover: parsing error at: %d\n", next_trans_start);
		return (error);
	}

	printf("ext2_journal_recover: recovery end\n");
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
	jrnp->jrn_blocksize = jrnp->jrn_sb->jsb_blocksize;
	jrnp->jrn_max_blocks = jrnp->jrn_sb->jsb_max_blocks;
	jrnp->jrn_first = jrnp->jrn_sb->jsb_first_block;
	jrnp->jrn_last = jrnp->jrn_first + jrnp->jrn_max_blocks - 1;

	if (jrnp->jrn_max_blocks < EXT2_JOURNAL_MIN_BLOCKS) {
		printf("ext2fs: journal number of blocks too little\n");
		return (EINVAL);
	}

	if (le16toh(jrnp->jrn_fs->e2fs->e2fs_state) & E2FS_ISCLEAN) {
		jrnp->jrn_flags |= EXT2_JOURNAL_CLEAN;
	} else {
		jrnp->jrn_flags |= EXT2_JOURNAL_NEEDS_RECOVERY;
	}

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

	*jrnpp = malloc(sizeof(struct ext2fs_journal), M_EXT2JOURNAL,
	    M_WAITOK | M_ZERO);

	error = ext2_journal_open_inode(mp, &((*jrnpp)->jrn_vp),
                              &((*jrnpp)->jrn_sb));
	if (error != 0) {
		printf("ext2fs: failed to open journal inode. error: %d\n", error);
		ext2_journal_close(*jrnpp);
		*jrnpp = NULL;
		return (error);
	}

	(*jrnpp)->jrn_fs = fs;
	error = ext2_journal_init(*jrnpp);
	if (error != 0) {
		printf("ext2fs: failed initialize journal. error: %d\n", error);
		ext2_journal_close(*jrnpp);
		*jrnpp = NULL;
		return (error);
	}
	ump->um_journal = *jrnpp;
	return (0);
}
