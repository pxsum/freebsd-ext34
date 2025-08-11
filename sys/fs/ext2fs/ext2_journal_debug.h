/*
 * Debug macros for ext2 journal structures and more useful printing
 */

#include <fs/ext2fs/inode.h>


/*
 * Definitions for the buffer free lists.
 */
#define QUEUE_NONE	0	/* on no queue */
#define QUEUE_EMPTY	1	/* empty buffer headers */
#define QUEUE_DIRTY	2	/* B_DELWRI buffers */
#define QUEUE_CLEAN	3	/* non-B_DELWRI buffers */
#define QUEUE_SENTINEL	4	/* not an queue index, but mark for sentinel */

#if 1
#define EXT2_JPRINTF(fmt, ...) \
    printf("EXT2J: %s(): " fmt, __func__, ##__VA_ARGS__)

#define EXT2_JPRINT_JOURNAL(jrnp) do { \
    if (jrnp) { \
        printf("\n=== EXT2 JOURNAL STATE ===\n"); \
        printf("vnode:           %p\n", (jrnp)->jrn_vp); \
        printf("flags:           0x%x", (jrnp)->jrn_flags); \
        if ((jrnp)->jrn_flags & EXT2_JOURNAL_CLEAN) \
            printf(" (CLEAN)"); \
        else if ((jrnp)->jrn_flags & EXT2_JOURNAL_NEEDS_RECOVERY) \
            printf(" (NEEDS_RECOVERY)"); \
        printf("\n"); \
        printf("blocksize:       %u\n", (jrnp)->jrn_blocksize); \
        printf("max_blocks:      %u\n", (jrnp)->jrn_max_blocks); \
        printf("free_blocks:     %u\n", (jrnp)->jrn_free_blocks); \
        printf("block_range:     %u - %u\n", (jrnp)->jrn_first, (jrnp)->jrn_last); \
        printf("log_range:       start=%u, end=%u\n", (jrnp)->jrn_log_start, (jrnp)->jrn_log_end); \
        printf("sequence:        %u\n", (jrnp)->jrn_sequence); \
        printf("active_trans:    %p\n", (jrnp)->jrn_active_trans); \
        printf("committing_trans:%p\n", (jrnp)->jrn_committing_trans); \
        printf("==============================\n\n"); \
    } else { \
        printf("EXT2J: NULL journal pointer\n"); \
    } \
} while (0)

#define EXT2_JPRINT_TRANS(trans) do { \
    if (trans) { \
        const char *state_str; \
        switch ((trans)->jt_state) { \
        case EXT2_TRANS_RUNNING: state_str = "RUNNING"; break; \
        case EXT2_TRANS_FLUSH:   state_str = "FLUSH";   break; \
        default:                 state_str = "UNKNOWN"; break; \
        } \
        printf("\n=== TRANSACTION STATE ===\n"); \
        printf("pointer:         %p\n", (trans)); \
        printf("state:           %s (%d)\n", state_str, (trans)->jt_state); \
        printf("refcount:        %d\n", (trans)->jt_refcount); \
        printf("owner_thread:    %p\n", (trans)->jt_owner); \
        printf("blocks_used:     %d\n", (trans)->jt_blocks_used); \
        printf("blocks_reserved: %d\n", (trans)->jt_blocks_reserved); \
        printf("data_buffers:    %d\n", (trans)->jt_data_count); \
        printf("metadata_buffers:%d\n", (trans)->jt_metadata_count); \
        printf("==========================\n\n"); \
    } else { \
        printf("EXT2J: NULL transaction pointer\n"); \
    } \
} while (0)

#define EXT2_JPRINT_JBUF(jbuf) do { \
    if (jbuf) { \
        const char *type_str; \
        if ((jbuf)->jb_type == EXT2_JBUF_DATA) \
            type_str = "DATA"; \
        else if ((jbuf)->jb_type == EXT2_JBUF_METADATA) \
            type_str = "METADATA"; \
        else \
            type_str = "UNKNOWN"; \
        printf("\n=== JOURNAL BUFFER ===\n"); \
        printf("jbuf_ptr:    %p\n", (jbuf)); \
        printf("type:        %s\n", type_str); \
        printf("block_num:   %u\n", (jbuf)->jb_blocknr); \
        printf("buf_ptr:     %p\n", (jbuf)->jb_buf); \
        printf("owner_trans: %p\n", (jbuf)->jb_owning_trans); \
        if ((jbuf)->jb_buf) { \
            printf("buf_lblkno:  %ld\n", (jbuf)->jb_buf->b_lblkno); \
            printf("buf_flags:   0x%x\n", (jbuf)->jb_buf->b_flags); \
        } \
        printf("=======================\n\n"); \
    } else { \
        printf("EXT2J: NULL jbuf pointer\n"); \
    } \
} while (0)

#define EXT2_JPRINT_BUF(bp) do { \
    if (bp) { \
        printf("\n=== BUFFER INFO ===\n"); \
        printf("buf_ptr:       %p\n", (bp)); \
        printf("b_lblkno:      %ld (logical block)\n", (bp)->b_lblkno); \
        printf("b_blkno:       %ld (physical block)\n", (bp)->b_blkno); \
        printf("b_flags:       0x%x\n", (bp)->b_flags); \
        printf("b_xflags:      0x%x\n", (bp)->b_xflags); \
        printf("b_vflags:      0x%x\n", (bp)->b_vflags); \
        printf("b_qindex:      %u\n", (bp)->b_qindex); \
        printf("b_bcount:      %ld\n", (bp)->b_bcount); \
        printf("b_bufsize:     %ld\n", (bp)->b_bufsize); \
        printf("b_vp:          %p\n", (bp)->b_vp); \
        printf("b_vp:          %p\n", (bp)->b_vp); \
	printf("b_vp_inum:     %lu\n", (uint64_t) VTOI((bp)->b_vp)->i_number); \
        printf("==================\n\n"); \
    } else { \
        printf("EXT2J: NULL buf pointer\n"); \
    } \
} while (0)

#define EXT2_JPRINT_JSB(jsb) do { \
    if (jsb) { \
        printf("\n=== JOURNAL SUPERBLOCK ===\n"); \
        printf("magic:           0x%x", be32toh((jsb)->jsb_header.jbh_magic)); \
        if (be32toh((jsb)->jsb_header.jbh_magic) == EXT2_JOURNAL_MAGIC) \
            printf(" (VALID)"); \
        else \
            printf(" (INVALID)"); \
        printf("\n"); \
        printf("blocktype:       %u\n", be32toh((jsb)->jsb_header.jbh_blocktype)); \
        printf("sequence:        %u\n", be32toh((jsb)->jsb_header.jbh_sequence_num)); \
        printf("blocksize:       %u\n", be32toh((jsb)->jsb_blocksize)); \
        printf("max_blocks:      %u\n", be32toh((jsb)->jsb_max_blocks)); \
        printf("first_block:     %u\n", be32toh((jsb)->jsb_first_block)); \
        printf("sequence_id:     %u\n", be32toh((jsb)->jsb_sequence_id)); \
        printf("start_block:     %u\n", be32toh((jsb)->jsb_start_block_num)); \
        printf("errno:           %u\n", be32toh((jsb)->jsb_errno)); \
        printf("compat:          0x%x\n", be32toh((jsb)->jsb_feature_compat)); \
        printf("incompat:        0x%x\n", be32toh((jsb)->jsb_feature_incompat)); \
        printf("ro_compat:       0x%x\n", be32toh((jsb)->jsb_feature_ro_compat)); \
        printf("num_users:       %u\n", be32toh((jsb)->jsb_num_users)); \
        printf("===========================\n\n"); \
    } else { \
        printf("EXT2J: NULL jsb pointer\n"); \
    } \
} while (0)

#define EXT2_JPRINT_BUFLIST(head, name) do { \
    struct ext2fs_journal_buf *jbuf; \
    int count = 0; \
    printf("\n=== %s BUFFER LIST ===\n", name); \
    TAILQ_FOREACH(jbuf, (head), jb_list) { \
        printf("[%d] jbuf=%p id=%d block=%u buf=%p inum=%lu\n", \
               count++, jbuf, jbuf->jb_id, jbuf->jb_blocknr, jbuf->jb_buf, (uint64_t) VTOI(jbuf->jb_buf->b_vp)->i_number); \
    } \
    if (count == 0) { \
        printf("(empty list)\n"); \
    } \
    printf("Total: %d buffers\n", count); \
    printf("=========================\n\n"); \
} while (0)

#define EXT2_JPRINT_TRANS_BUFFERS(trans) do { \
    if (trans) { \
        printf("\n=== TRANSACTION BUFFERS ===\n"); \
        printf("Transaction: %p\n", (trans)); \
        EXT2_JPRINT_BUFLIST(&(trans)->jt_data_buffers, "DATA"); \
        EXT2_JPRINT_BUFLIST(&(trans)->jt_metadata_buffers, "METADATA"); \
        printf("============================\n\n"); \
    } else { \
        printf("EXT2J: NULL transaction pointer\n"); \
    } \
} while (0)

/* Journal block header */
#define EXT2_JPRINT_BLOCK_HEADER(hdr) do { \
    if (hdr) { \
        const char *type_str; \
        uint32_t type = be32toh((hdr)->jbh_blocktype); \
        switch (type) { \
        case EXT2_JOURNAL_DESCRIPTOR_BLOCK:  type_str = "DESCRIPTOR"; break; \
        case EXT2_JOURNAL_COMMIT_BLOCK:      type_str = "COMMIT"; break; \
        case EXT2_JOURNAL_REVOKE_BLOCK:      type_str = "REVOKE"; break; \
        case EXT2_JOURNAL_FORMAT_BASIC:      type_str = "SUPERBLOCK_BASIC"; break; \
        case EXT2_JOURNAL_FORMAT_EXTENDED:   type_str = "SUPERBLOCK_EXTENDED"; break; \
        default:                             type_str = "UNKNOWN"; break; \
        } \
        printf("\n=== JOURNAL BLOCK HEADER ===\n"); \
        printf("magic:    0x%x", be32toh((hdr)->jbh_magic)); \
        if (be32toh((hdr)->jbh_magic) == EXT2_JOURNAL_MAGIC) \
            printf(" (VALID)"); \
        else \
            printf(" (INVALID)"); \
        printf("\n"); \
        printf("type:     %s (%u)\n", type_str, type); \
        printf("sequence: %u\n", be32toh((hdr)->jbh_sequence_num)); \
        printf("=============================\n\n"); \
    } else { \
        printf("EXT2J: NULL block header pointer\n"); \
    } \
} while (0)

#define EXT2_JPRINT_STATUS(jrnp) do { \
    if (jrnp) { \
        printf("EXT2J_STATUS: seq=%u log=[%u-%u] active=%p commit=%p\n", \
               (jrnp)->jrn_sequence, (jrnp)->jrn_log_start, (jrnp)->jrn_log_end, \
               (jrnp)->jrn_active_trans, (jrnp)->jrn_committing_trans); \
    } else { \
        printf("EXT2J_STATUS: NULL journal\n"); \
    } \
} while (0)

#define EXT2_JTRACE_ENTER() \
    printf("EXT2J: >>> %s() ENTER\n", __func__)

#define EXT2_JTRACE_EXIT(ret) \
    printf("EXT2J: <<< %s() EXIT (ret=%d)\n", __func__, (ret))

#define EXT2_JERROR(fmt, ...) \
    printf("EXT2J_ERROR: %s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__)

#else
#define EXT2_JPRINTF(fmt, ...)
#define EXT2_JPRINT_JOURNAL(jrnp)
#define EXT2_JPRINT_TRANS(trans)
#define EXT2_JPRINT_JBUF(jbuf)
#define EXT2_JPRINT_JSB(jsb)
#define EXT2_JPRINT_BUFLIST(head, name)
#define EXT2_JPRINT_TRANS_BUFFERS(trans)
#define EXT2_JPRINT_BLOCK_HEADER(hdr)
#define EXT2_JPRINT_STATUS(jrnp)
#define EXT2_JTRACE_ENTER()
#define EXT2_JTRACE_EXIT(ret)
#define EXT2_JERROR(fmt, ...)
#endif
