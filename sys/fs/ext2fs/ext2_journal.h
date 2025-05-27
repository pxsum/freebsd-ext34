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

#ifndef _FS_EXT2FS_EXT2_JOURNAL_H_
#define _FS_EXT2FS_EXT2_JOURNAL_H_

#include <sys/types.h>
#include <fs/ext2fs/inode.h>

#define	EXT2_JOURNAL_MAGIC 0xc03b3998
#define	EXT2_JOURNAL_MIN_BLOCKS 1024


/*
 * The following structures represent the on-disk journal format.
 * All fields are stored in big-endian byte order on disk.
 */

/*
 * Journal block types
 */
enum journal_block_type {
	EXT2_JOURNAL_DESCRIPTOR_BLOCK,	/* descriptor data blocks */
	EXT2_JOURNAL_COMMIT_BLOCK,	/* indicates transaction completion */
	EXT2_JOURNAL_FORMAT_BASIC,	/* basic journal superblock format */
	EXT2_JOURNAL_FORMAT_EXTENDED,	/* extended journal superblock */
	EXT2_JOURNAL_REVOKE_BLOCK	/* block revocation records */
};

/*
 * Journal checksum types
 */
enum journal_checksum_type {
	EXT2_JOURNAL_CHECKSUM_CRC32,
	EXT2_JOURNAL_CHECKSUM_MD5,
	EXT2_JOURNAL_CHECKSUM_SHA1,
	EXT2_JOURNAL_CHECKSUM_CRC32C
};

/*
 * Every block in the journal starts with this header
 */
struct ext2fs_journal_block_header {
	uint32_t jbh_magic;		/* journal magic number */
	uint32_t jbh_blocktype;		/* type of block */
	uint32_t jbh_sequence_num;	/* sequence number */
};

/*
 * Journal superblock
 */
struct ext2fs_journal_sb {
	struct ext2fs_journal_block_header jsb_header;/* common header */
	uint32_t jsb_blocksize;		/* device block size */
	uint32_t jsb_max_blocks;	/* total blocks in this journal */
	uint32_t jsb_first_block;	/* first block of log */
	uint32_t jsb_sequence_id;	/* first commit id */
	uint32_t jsb_start_block_num;	/* first block of the start of log */
	uint32_t jsb_errno;		/* error value */
	uint32_t jsb_feature_compat;	/* compatiable features */
	uint32_t jsb_feature_incompat;	/* incompatiable features */
	uint32_t jsb_feature_ro_compat;	/* read-only compatiable features */
	uint8_t  jsb_uuid[16];		/* 128-bit uuid for journal */
	uint32_t jsb_num_users;		/* # of filesystems sharing journal */
	uint32_t jsb_dynamic_sb;	/* block # of dynamic SB copy */
	uint32_t jsb_trans_max;		/* max # blocks per transaction */
	uint32_t jsb_trans_data_max;	/* max # data blocks per transaction */
	uint32_t jsb_checksum_type;	/* checksum algorithm */
	uint8_t  jsb_padding2[3];
	uint32_t jsb_num_fc_blocks;	/* # of fast commit blocks in journal */
};

struct ext2fs_journal_block_tail {
	uint32_t jbt_checksum;
};

/*
 * Revoke blocks list blocks that should not be replayed during recovery
 */
struct ext2fs_journal_revoke_header {
	struct ext2fs_journal_block_header jrh_header;
	uint32_t jrh_size;	/* size of the revoke data */
};

/*
 * Used for verifying revoke block integrity
 */
struct ext2fs_journal_revoke_tail {
	uint32_t jrt_checksum;
};

#define	JOURNAL_COMMIT_CHECKSUM_SIZE (32)

/*
 * A commit block marks the end of a complete transaction in the journal
 */
struct ext2fs_journal_commit_header {
	struct ext2fs_journal_block_header jch_header;
	uint8_t jch_checksum_type;	/* type of checksum used */
	uint8_t jch_checksum_size;	/* size of checksum */
	uint8_t jch_padding[2];
	uint32_t jch_checksum[JOURNAL_COMMIT_CHECKSUM_SIZE];
	uint64_t jch_timestamp_sec;	/* commit time in secs */
	uint32_t jch_timestamp_nsec;	/* commit time in nanosecs */
};

/*
 * When loaded into memory, journal structures are converted to
 * host native byte order.
 */

enum ext2_journal_state {
	EXT2_JOURNAL_CLEAN,
	EXT2_JOURNAL_NEEDS_RECOVERY
};

struct vnode;
struct m_ext2fs;
// TODO
struct ext2fs_journal_transaction;

struct ext2fs_journal {
	struct vnode *jrn_vp;
	struct m_ext2fs *jrn_fs;
	struct ext2fs_journal_sb *jrn_sb;
	struct ext2fs_journal_transaction *jrn_active_trans;
	struct ext2fs_journal_transaction *jrn_committing_trans;

	uint32_t     jrn_flags;
	size_t       jrn_blocksize;
	blkcnt_t     jrn_max_blocks;
	blkcnt_t     jrn_free_blocks;
	e4fs_daddr_t jrn_first;
	e4fs_daddr_t jrn_last;
	e4fs_daddr_t jrn_log_start;
	e4fs_daddr_t jrn_log_end;
};


int ext2_journal_open(struct mount *mp, struct ext2fs_journal **jrnpp);
int ext2_journal_close(struct ext2fs_journal *jrnp);

#endif	/* !_FS_EXT2FS_EXT2_JOURNAL_H_ */
