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

MALLOC_DEFINE(M_EXT2JOURNAL, "ext2fs_journal", "In-memory ext2 journal");
MALLOC_DEFINE(M_EXT2JSB, "ext2fs_journal_sb", "In-memory copy of \
	journal superblock");

/*
 * Verfies if the given data block is a valid journal block.
 */
static bool
ext2_verify_journal_block(void *data)
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
	if (!ext2_verify_journal_block(jrn_data)) {
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

	memcpy(*jrn_sbpp, jrn_sbp, sizeof(struct ext2fs_journal_sb));

	brelse(jrn_buf);
	VOP_UNLOCK(*vpp);
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
	jrnp->jrn_blocksize = be32toh(jrnp->jrn_sb->jsb_blocksize);
	jrnp->jrn_max_blocks = be32toh(jrnp->jrn_sb->jsb_max_blocks);
	jrnp->jrn_first = be32toh(jrnp->jrn_sb->jsb_first_block);
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
