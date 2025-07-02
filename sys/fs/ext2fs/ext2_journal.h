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

#include <sys/queue.h>

#define	EXT2_JOURNAL_MAGIC 0xc03b3998
#define	EXT2_JOURNAL_MIN_BLOCKS 1024

#define EXT2_JOURNAL_INCOMPAT_REVOKE		(1)
#define EXT2_JOURNAL_INCOMPAT_64BIT		(2)
#define EXT2_JOURNAL_INCOMPAT_ASYNC_COMMIT	(4)
#define EXT2_JOURNAL_INCOMPAT_CHECKSUM_V2	(8)

/*
 * The following structures represent the on-disk journal format.
 * All fields are stored in big-endian byte order on disk.
 */

/*
 * Defines the different block types and journaling version.
 */
enum journal_block_type {
	EXT2_JOURNAL_DESCRIPTOR_BLOCK = 1,	/* descriptor data blocks */
	EXT2_JOURNAL_COMMIT_BLOCK,	/* indicates transaction completion */

	/* Journaling versions */
	EXT2_JOURNAL_FORMAT_BASIC,	/* basic journal superblock format */
	EXT2_JOURNAL_FORMAT_EXTENDED,	/* extended journal superblock */

	EXT2_JOURNAL_REVOKE_BLOCK	/* block revocation records */
};

/*
 * Journal checksum types.
 */
enum journal_checksum_type {
	EXT2_JOURNAL_CHECKSUM_CRC32 = 1,
	EXT2_JOURNAL_CHECKSUM_MD5,
	EXT2_JOURNAL_CHECKSUM_SHA1,
	EXT2_JOURNAL_CHECKSUM_CRC32C
};

/*
 * Common header found at the beginning of every metablock in the journal.
 */
struct ext2fs_journal_block_header {
	uint32_t jbh_magic;		/* journal magic number */
	uint32_t jbh_blocktype;		/* type of block */
	uint32_t jbh_sequence_num;	/* sequence number */
};

/*
 * On-disk structure for the journal superblock.
 */
struct ext2fs_journal_sb {
	struct ext2fs_journal_block_header jsb_header;/* common header */
	uint32_t jsb_blocksize;		/* device block size */
	uint32_t jsb_max_blocks;	/* total blocks in this journal */
	uint32_t jsb_first_block;	/* static first block of log */
	uint32_t jsb_sequence_id;	/* first commit id */
	uint32_t jsb_start_block_num;	/* dynamic starting block of log */
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

#define EXT2_JOURNAL_TAG_ESCAPED         (1)
#define EXT2_JOURNAL_TAG_SAME_UUID       (2)
#define EXT2_JOURNAL_TAG_DELETED         (4)
#define EXT2_JOURNAL_TAG_LAST_ENTRY      (8)

struct ext2fs_journal_desc_tag {
	uint32_t  jdt_blocknum_low;	/* low bits of block num*/
	uint16_t  jdt_checksum;		/* checksum */
	uint16_t  jdt_flags;		/* flags for block */
};

struct ext2fs_journal_desc_tail {
	uint32_t jbt_checksum;
};

/*
 * Revoke blocks list blocks that should not be replayed during recovery.
 */
struct ext2fs_journal_revoke_header {
	struct ext2fs_journal_block_header jrh_header;
	uint32_t jrh_size;	/* size of the revoke data */
};

/*
 * Used for verifying revoke block integrity.
 */
struct ext2fs_journal_revoke_tail {
	uint32_t jrt_checksum;
};

#define	JOURNAL_COMMIT_CHECKSUM_SIZE (32)

/*
 * A commit block marks the end of a complete transaction in the journal.
 */
struct ext2fs_journal_commit_header {
	struct ext2fs_journal_block_header jch_header;
	uint8_t  jch_checksum_type;	/* type of checksum used */
	uint8_t  jch_checksum_size;	/* size of checksum */
	uint8_t  jch_padding[2];
	uint32_t jch_checksum[JOURNAL_COMMIT_CHECKSUM_SIZE];
	uint64_t jch_timestamp_sec;	/* commit time in secs */
	uint32_t jch_timestamp_nsec;	/* commit time in nanosecs */
};

/* In-memory journal flags */
#define EXT2_JOURNAL_CLEAN		(0)
#define EXT2_JOURNAL_NEEDS_RECOVERY	(1)
#define EXT2_JOURNAL_ABORTED		(2)

enum ext2_journal_trans_state {
	EXT2_TRANS_RUNNING,
	EXT2_TRANS_COMMIT,
	EXT2_TRANS_FLUSH
};

enum ext2_journal_buf_type {
	EXT2_JBUF_DATA,
	EXT2_JBUF_METADATA
};

struct ext2_journal_buf {
	TAILQ_ENTRY(ext2_journal_buf) jb_list;

	struct ext2fs_journal_transaction *jb_owning_trans;
	struct buf *jb_buf;
	enum ext2_journal_buf_type jb_type;
	uint32_t jb_blocknr;
};

TAILQ_HEAD(ext2_journal_buf_list, ext2_journal_buf);
struct ext2fs_journal_transaction {
	struct ext2fs_journal *jt_journal;
	enum ext2_journal_trans_state jt_state;

	int jt_refcount;
	struct thread *jt_owner;

	int jt_blocks_used;
	int jt_blocks_reserved;

	/* Buffer lists for ordered journaling */
	struct ext2_journal_buf_list jt_data_buffers;
	struct ext2_journal_buf_list jt_metadata_buffers;
	int jt_data_count;
	int jt_metadata_count;

	/* List of checkpoint transaction */
	TAILQ_ENTRY(ext2fs_journal_transaction) jt_checkpoint_entry;

	int jt_pending_data;
	struct cv jt_iowait_cv;
};

struct vnode;
struct m_ext2fs;

/* In-memory representation of an active journal.
 *
 * The on-disk superblock is kept in big-endian while all other fields are in
 * host byte order.
 */
TAILQ_HEAD(ext2_journal_checkpoint_list, ext2fs_journal_transaction);
struct ext2fs_journal {
	struct vnode *jrn_vp;
	struct m_ext2fs *jrn_fs;
	struct ext2fs_journal_sb *jrn_sb;
	struct ext2fs_journal_transaction *jrn_active_trans;
	struct ext2fs_journal_transaction *jrn_committing_trans;
	/* List of transactions to checkpoint */
	struct ext2_journal_checkpoint_list jrn_checkpoint_list;

	bool		jrn_block_new_trans;

	uint32_t	jrn_flags;
	uint32_t	jrn_blocksize;
	uint32_t	jrn_max_blocks;
	uint32_t	jrn_free_blocks;
	uint32_t	jrn_first;	/* static first block of journal */
	uint32_t	jrn_last;	/* static last block of journal */
	uint32_t	jrn_log_start;	/* dynamic start of journal */
	uint32_t	jrn_log_end;	/* dynamic end of journal */
	uint32_t	jrn_sequence;

	struct mtx jrn_lock;
	struct cv jrn_trans_start_cv;
	struct cv jrn_trans_commit_cv; /* Wait for commit to complete */
	struct cv jrn_trans_block_cv; /* Prevents new transtions from start */
	struct cv jrn_space_cv; /* Wait for enough journal space */
};

int ext2_journal_open(struct mount *mp, struct ext2fs_journal **jrnpp);
int ext2_journal_close(struct ext2fs_journal *jrnp);
int ext2_journal_recover(struct ext2fs_journal *jrnp);

int ext2_journal_start(struct ext2fs_journal *jrnp, int nblocks);
int ext2_journal_stop(struct ext2fs_journal *jrnp);
int ext2_journal_dirty_metadata(struct ext2fs_journal *jrnp, struct buf *bp);
int ext2_journal_dirty_data(struct ext2fs_journal *jrnp, struct buf *bp);


void ext2_journal_block_new_tran(struct ext2fs_journal *jrnp);
int ext2_journal_force_commit(struct ext2fs_journal *jrnp);
int ext2_journal_checkpoint_trans(struct ext2fs_journal *jrnp);

#endif	/* !_FS_EXT2FS_EXT2_JOURNAL_H_ */
