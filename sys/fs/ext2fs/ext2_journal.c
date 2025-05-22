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
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/endian.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/mutex.h>

#include <fs/ext2fs/fs.h>
#include <fs/ext2fs/ext2fs.h>
#include <fs/ext2fs/ext2_mount.h>
#include <fs/ext2fs/inode.h>
#include <fs/ext2fs/ext2_journal.h>
#include <fs/ext2fs/ext2_dinode.h>

MALLOC_DECLARE(M_EXT2JOURNAL);
MALLOC_DEFINE(M_EXT2JOURNAL, "ext2fs_journal", "In-memory ext2 journal");


MALLOC_DECLARE(M_EXT2JSB);
MALLOC_DEFINE(M_EXT2JSB, "ext2fs_journal_sb", "In-memory copy of \
	journal superblock");

static bool
ext2_verify_journal_block(void *data)
{
	struct ext2fs_journal_block_header *jrn_bhr =
		(struct ext2fs_journal_block_header *) data;
	return (be32toh(jrn_bhr->jbh_magic) == EXT2_JOURNAL_MAGIC);
}

static void
ext2_jsb_from_disk(struct ext2fs_journal_sb *host_jsb,
    struct ext2fs_journal_sb *disk_jsb)
{
	host_jsb->jsb_header.jbh_magic = be32toh(disk_jsb->jsb_header.jbh_magic);
	host_jsb->jsb_header.jbh_blocktype = be32toh(disk_jsb->jsb_header.jbh_blocktype);
	host_jsb->jsb_header.jbh_sequence_num = be32toh(disk_jsb->jsb_header.jbh_sequence_num);
	host_jsb->jsb_blocksize = be32toh(disk_jsb->jsb_blocksize);
	host_jsb->jsb_max_blocks = be32toh(disk_jsb->jsb_max_blocks);
	host_jsb->jsb_first_block = be32toh(disk_jsb->jsb_first_block);
	host_jsb->jsb_sequence_id = be32toh(disk_jsb->jsb_sequence_id);
	host_jsb->jsb_start_block_num = be32toh(disk_jsb->jsb_start_block_num);
	host_jsb->jsb_errno = be32toh(disk_jsb->jsb_errno);
	host_jsb->jsb_feature_compat = be32toh(disk_jsb->jsb_feature_compat);
	host_jsb->jsb_feature_incompat = be32toh(disk_jsb->jsb_feature_incompat);
	host_jsb->jsb_feature_ro_compat = be32toh(disk_jsb->jsb_feature_ro_compat);
	host_jsb->jsb_num_users = be32toh(disk_jsb->jsb_num_users);
	host_jsb->jsb_dynamic_sb = be32toh(disk_jsb->jsb_dynamic_sb);
	host_jsb->jsb_trans_max = be32toh(disk_jsb->jsb_trans_max);
	host_jsb->jsb_trans_data_max = be32toh(disk_jsb->jsb_trans_data_max);
	host_jsb->jsb_checksum_type = be32toh(disk_jsb->jsb_checksum_type);

	memcpy(host_jsb->jsb_uuid, disk_jsb->jsb_uuid, 16);
	memcpy(host_jsb->jsb_padding2, disk_jsb->jsb_padding2, 3);
}

static void
ext2_jsb_to_disk(struct ext2fs_journal_sb *disk_jsb,
    struct ext2fs_journal_sb *host_jsb)
{
	disk_jsb->jsb_header.jbh_magic = htobe32(host_jsb->jsb_header.jbh_magic);
	disk_jsb->jsb_header.jbh_blocktype = htobe32(host_jsb->jsb_header.jbh_blocktype);
	disk_jsb->jsb_header.jbh_sequence_num = htobe32(host_jsb->jsb_header.jbh_sequence_num);
	disk_jsb->jsb_blocksize = htobe32(host_jsb->jsb_blocksize);
	disk_jsb->jsb_max_blocks = htobe32(host_jsb->jsb_max_blocks);
	disk_jsb->jsb_first_block = htobe32(host_jsb->jsb_first_block);
	disk_jsb->jsb_sequence_id = htobe32(host_jsb->jsb_sequence_id);
	disk_jsb->jsb_start_block_num = htobe32(host_jsb->jsb_start_block_num);
	disk_jsb->jsb_errno = htobe32(host_jsb->jsb_errno);
	disk_jsb->jsb_feature_compat = htobe32(host_jsb->jsb_feature_compat);
	disk_jsb->jsb_feature_incompat = htobe32(host_jsb->jsb_feature_incompat);
	disk_jsb->jsb_feature_ro_compat = htobe32(host_jsb->jsb_feature_ro_compat);
	disk_jsb->jsb_num_users = htobe32(host_jsb->jsb_num_users);
	disk_jsb->jsb_dynamic_sb = htobe32(host_jsb->jsb_dynamic_sb);
	disk_jsb->jsb_trans_max = htobe32(host_jsb->jsb_trans_max);
	disk_jsb->jsb_trans_data_max = htobe32(host_jsb->jsb_trans_data_max);
	disk_jsb->jsb_checksum_type = htobe32(host_jsb->jsb_checksum_type);

	memcpy(disk_jsb->jsb_uuid, host_jsb->jsb_uuid, 16);
	memcpy(disk_jsb->jsb_padding2, host_jsb->jsb_padding2, 3);
}

static int
ext2_journal_open_inode(struct mount *mp, struct vnode **vpp,
    struct ext2fs_journal_sb **jrn_sbpp) {
	struct buf *jrn_buf;
	void *jrn_data;
	struct ext2mount *ump = VFSTOEXT2(mp);
	struct m_ext2fs *fs = ump->um_e2fs;
	struct ext2fs_journal_sb *jrn_sbp;

	// not sure what flag to pass in
	int result = VFS_VGET(mp, EXT2_JOURNALINO, 0, vpp);
	if (result != 0) {
		*vpp = NULL;
		return result;
	}
	result = bread(*vpp, 0, (int) fs->e2fs_bsize, NOCRED, &jrn_buf);
	if (result != 0) {
		vput(*vpp);
		*vpp = NULL;
		return result;
	}

	jrn_data = jrn_buf->b_data;
	if (!ext2_verify_journal_block(jrn_data)) {
		brelse(jrn_buf);
		vput(*vpp);
		*vpp = NULL;
		return EINVAL;
	}

	jrn_sbp = (struct ext2fs_journal_sb *) jrn_data;
	if (be32toh(jrn_sbp->jsb_header.jbh_blocktype) !=
	    EXT2_JOURNAL_FORMAT_BASIC &&
	    be32toh(jrn_sbp->jsb_header.jbh_blocktype) !=
	    EXT2_JOURNAL_FORMAT_EXTENDED)
	    {
		    brelse(jrn_buf);
		    vput(*vpp);
		    *vpp = NULL;
		    return EINVAL;
	    }

	*jrn_sbpp = (struct ext2fs_journal_sb *)
		malloc(sizeof(struct ext2fs_journal_sb), M_EXT2JSB, M_WAITOK);

	ext2_jsb_from_disk(*jrn_sbpp, jrn_sbp);
	brelse(jrn_buf);
	VOP_UNLOCK(*vpp);
	return (0);
}

// TODO
static int
ext2_journal_init(struct ext2fs_journal *jrnp)
{
	return 0;
}

// TODO
static int
ext2_journal_close(struct ext2fs_journal *jrnp)
{
	return 0;
}

static int
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
		free(*jrnpp, M_EXT2JOURNAL);
		*jrnpp = NULL;
		return (error);
	}

	(*jrnpp)->jrn_fs = fs;
	error = ext2_journal_init(*jrnpp); // TODO
	if (error != 0) {
		ext2_journal_close(*jrnpp); // TODO
		return (error);
	}

	return (0);
}
