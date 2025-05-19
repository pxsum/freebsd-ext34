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
#include <stdint.h>

#define JOURNAL_MAGIC 0xc03b3998

/*
 * Journal block types
 */
enum journal_block_type {
    EXT2_JOURNAL_DESCRIPTOR_BLOCK    = 1, /* descriptor data blocks */
    EXT2_JOURNAL_COMMIT_BLOCK        = 2, /* indicates transaction completion */
    EXT2_JOURNAL_FORMAT_BASIC        = 3, /* basic journal superblock format */
    EXT2_JOURNAL_FORMAT_EXTENDED     = 4, /* extended journal superblock */
    EXT2_JOURNAL_REVOKE_BLOCK        = 5  /* block revocation records */
};

/*
 * Journal checksum types
 */
enum journal_checksum_type {
    EXT2_JOURNAL_CHECKSUM_CRC32      = 1,
    EXT2_JOURNAL_CHECKSUM_MD5        = 2,
    EXT2_JOURNAL_CHECKSUM_SHA1       = 3,
    EXT2_JOURNAL_CHECKSUM_CRC32C     = 4
};

/*
 * Every block in the journal starts with this header
 */
struct ext2fs_journal_block_header {
	uint32_t jh_magic;            /* journal magic number */
	uint32_t jh_blocktype;        /* type of block */
	uint32_t jh_sequence_num;     /* sequence number */
};

/*
 * Journal superblock
 */
struct ext2fs_journal_sb {
	struct ext2fs_journal_block_header jsb_header; /* common header */
	uint32_t jsb_blocksize;          /* device block size */
	uint32_t jsb_max_blocks;         /* total blocks in this journal */
	uint32_t jsb_first_block;        /* first block of log */
	uint32_t jsb_sequence_id;        /* first commit id */
	uint32_t jsb_start_block_num;    /* first block of the start of log */
	uint32_t jsb_errno;              /* error value */
	uint32_t jsb_feature_compat;     /* compatiable features */
	uint32_t jsb_feature_incompat;   /* incompatiable features */
	uint32_t jsb_feature_ro_compat;  /* read-only compatiable features */
	uint8_t  jsb_uuid[16];           /* 128-bit uuid for journal */
	uint32_t jsb_num_users;          /* # of filesystems sharing journal */
	uint32_t jsb_dynamic_sb;         /* block # of dynamic SB copy */
	uint32_t jsb_trans_max;          /* max # blocks per transaction */
	uint32_t jsb_trans_data_max;     /* max # data blocks per transaction */
	uint32_t jsb_checksum_type;      /* checksum algorithm */
	uint8_t  jsb_padding2[3];
	uint32_t jsb_num_fc_blocks;      /* # of fast commit blocks in journal */
	uint32_t jsb_head;               /* block # of the head */
	uint8_t  jsb_padding1[40];
	uint32_t jsb_checksum;
	uint8_t  jsb_user_ids[16 * 48];  /* filesystem identifiers */
};

/*
 * Each block in a transaction is described by a tag in the descriptor block
 */
struct ext2fs_journal_block_tag {
	uint32_t jdt_blockno;         /* block # in the main filesystem */
	uint16_t jdt_checksum;        /* checksum for block data integrity */
	uint16_t jdt_flags;           /* tag flags */
	uint32_t jdt_blockno_high;    /* high 32-bits of block # */
};

/*
 * Used at the end of a descriptor block for integrity checking
 */
struct ext2fs_journal_block_tail {
	uint32_t jbt_checksum;
};

/*
 * Revoke blocks list blocks that should not be replayed during recovery
 */
struct ext2fs_journal_revoke_header {
	struct ext2fs_journal_block_header jrh_header;
	uint32_t jrh_size;     /* size of the revoke data */
};

/*
 * Used for verifying revoke block integrity
 */
struct ext2fs_journal_revoke_tail {
	uint32_t jrt_checksum;
};

#define JOURNAL_COMMIT_CHECKSUM_SIZE (32)

/*
 * A commit block marks the end of a complete transaction in the journal
 */
struct ext2fs_journal_commit_header {
	struct ext2fs_journal_block_header jch_header;
	uint8_t               jch_checksum_type;   /* type of checksum used */
	uint8_t               jch_checksum_size;   /* size of checksum */
	uint8_t               jch_padding[2];
	uint32_t              jch_checksum[JOURNAL_COMMIT_CHECKSUM_SIZE];
	uint64_t              jch_timestamp_sec;   /* commit time in secs */
	uint32_t              jch_timestamp_nsec;  /* commit time in nanosecs */
};

#endif /* !_FS_EXT2FS_EXT2_JOURNAL_H_ */
