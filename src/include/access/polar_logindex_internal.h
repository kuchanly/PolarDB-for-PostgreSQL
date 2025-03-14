#ifndef POLAR_LOG_INDEX_INTERNAL_H
#define POLAR_LOG_INDEX_INTERNAL_H

#include "access/rmgr.h"
#include "access/slru.h"
#include "access/xlogrecord.h"
#include "lib/bloomfilter.h"
#include "storage/buf_internals.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/polar_fd.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/palloc.h"
#include "utils/pg_crc.h"


/* POLAR: logindex snapshot type */
typedef enum
{
	LOGINDEX_WAL_SNAPSHOT,
	LOGINDEX_FULLPAGE_SNAPSHOT,
	/* Maybe we have more type later */
	LOGINDEX_SNAPSHOT_NUM
} logindex_snapshot_type_t;

struct log_index_snapshot_t;
typedef struct log_index_snapshot_t     *logindex_snapshot_t;
extern struct log_index_snapshot_t *polar_logindex_snapshot[LOGINDEX_SNAPSHOT_NUM];

extern int polar_logindex_mem_tbl_size;

#define POLAR_LOGINDEX_WAL_SNAPSHOT (polar_logindex_snapshot[LOGINDEX_WAL_SNAPSHOT])
#define POLAR_LOGINDEX_FULLPAGE_SNAPSHOT (polar_logindex_snapshot[LOGINDEX_FULLPAGE_SNAPSHOT])

#define LOG_INDEX_DIR                   "pg_logindex"
#define LOG_INDEX_MAGIC                 (0xFDFE)
#define LOG_INDEX_VERSION               (0x0002)

#define POLAR_DATA_DIR() (POLAR_FILE_IN_SHARED_STORAGE() ? polar_datadir : DataDir)
#define POLAR_FILE_PATH(path, orign) \
	snprintf((path), MAXPGPATH, "%s/%s", POLAR_DATA_DIR(), (orign));

#define LOG_INDEX_FILE_TABLE_NAME(path, seg) \
	snprintf((path), MAXPGPATH, "%s/%s/%04lX.tbl", POLAR_DATA_DIR(), logindex_snapshot->dir, seg);

#define LOG_INDEX_META_FILE (logindex_snapshot->meta_file_name)

/* Define macro for const config value */
#define LOG_INDEX_MEM_TBL_SEG_NUM           4096
#define LOG_INDEX_MEM_TBL_HASH_NUM          (LOG_INDEX_MEM_TBL_SEG_NUM/2)
#define LOG_INDEX_MEM_TBL_HASH_LOCK_NUM     (LOG_INDEX_MEM_TBL_HASH_NUM/64)
#define LOG_INDEX_MEM_TBL_HASH_PAGE(tag) \
	(tag_hash(tag, sizeof(BufferTag)) % LOG_INDEX_MEM_TBL_HASH_NUM)

#define LOG_INDEX_TABLE_INVALID_ID          0
#define LOG_INDEX_TBL_INVALID_SEG           0

#define LOG_INDEX_ITEM_HEAD_LSN_NUM         2
#define LOG_INDEX_ITEM_SEG_LSN_NUM          10
#define LOG_INDEX_TBL_SEG_SIZE              48
#define LOG_INDEX_MAX_ORDER_NUM             (LOG_INDEX_MEM_TBL_SEG_NUM * LOG_INDEX_ITEM_SEG_LSN_NUM)

#define LOG_INDEX_FILE_TBL_BLOOM_SIZE       (4096)
#define LOG_INDEX_FILE_TBL_TOTAL_NUM \
	(SLRU_PAGES_PER_SEGMENT*BLCKSZ/LOG_INDEX_FILE_TBL_BLOOM_SIZE)
#define LOG_INDEX_BLOOM_ELEMS_NUM           (LOG_INDEX_MEM_TBL_SEG_NUM * 0.2)

/* Define macro to set and get active table id */
#define LOG_INDEX_MEM_TBL_ACTIVE_ID (logindex_snapshot->active_table)
#define LOG_INDEX_MEM_TBL_ACTIVE() (LOG_INDEX_MEM_TBL(LOG_INDEX_MEM_TBL_ACTIVE_ID))

#define LOG_INDEX_MEM_TBL_NEXT_ID(id) \
	(((id) == (logindex_snapshot->mem_tbl_size - 1)) ? 0 : ((id) + 1))

#define LOG_INDEX_MEM_TBL_PREV_ID(id) \
	(((id) == 0) ? (logindex_snapshot->mem_tbl_size - 1) : ((id) -1))

/* Define macro to get memory table */
#define LOG_INDEX_MEM_TBL(t) &(logindex_snapshot->mem_table[(t)])

/* Define macro to get item from memory table */
#define LOG_INDEX_ITEM_HEAD(t, seg) \
	(((seg) == LOG_INDEX_TBL_INVALID_SEG) ? NULL : \
	 &((t)->segment[(seg)-1].item_head))

#define LOG_INDEX_MEM_ITEM_IS(item, page_tag) \
	(memcmp(&(item)->tag, page_tag, sizeof(BufferTag)) == 0)

#define LOG_INDEX_ITEM_SEG(t, seg) \
	(((seg) == LOG_INDEX_TBL_INVALID_SEG) ? NULL : \
	 &((t)->segment[(seg)-1].item_seg))

/* Define macro to get and set hash slot */
#define LOG_INDEX_TBL_SLOT(t, key) \
	(&(t)->hash[(key)])

#define LOG_INDEX_TBL_SLOT_VALUE(t, key) \
	((t)->hash[(key)])

#define LOG_INDEX_COMBINE_LSN(table, suffix) \
	((((XLogRecPtr)((table)->prefix_lsn)) << 32) | (suffix))

#define LOG_INDEX_SAME_TABLE_LSN_PREFIX(table, lsn)   ((table)->prefix_lsn == ((lsn) >> 32))

/* Define macro to check item property */
#define LOG_INDEX_SEG_MIN_LSN(table, seg)    LOG_INDEX_COMBINE_LSN(table, (seg)->suffix_lsn[0])
#define LOG_INDEX_SEG_MAX_LSN(table, seg)    LOG_INDEX_COMBINE_LSN(table, (seg)->suffix_lsn[(seg)->number - 1])

/* Define macro to operate memory table property */
#define LOG_INDEX_MEM_TBL_STATE(t) pg_atomic_read_u32(&((t)->state))
#define LOG_INDEX_MEM_TBL_SET_STATE(t, s) pg_atomic_write_u32(&((t)->state), s)
#define LOG_INDEX_MEM_TBL_SET_PREFIX_LSN(table, lsn) \
	{ \
		(table)->data.prefix_lsn = ((lsn) >> 32) ; \
	}
#define LOG_INDEX_MEM_TBL_TID(t) (t)->data.idx_table_id

#define LOG_INDEX_MEM_TBL_FREE_HEAD(t) \
	((t)->free_head)

#define LOG_INDEX_MEM_TBL_UPDATE_FREE_HEAD(t) \
	((t)->free_head++)

/*
 * When we get a new active table, we don't know its data.prefix_lsn,
 * we assign InvalidXLogRecPtr lsn to data.prefix_lsn, so we should
 * distinguish which table without prefix_lsn, and reassign it
 */
#define LOG_INDEX_MEM_TBL_IS_NEW(active) \
	((active)->data.max_lsn == InvalidXLogRecPtr && \
	 (active)->data.min_lsn == UINT64_MAX && \
	 (active)->data.prefix_lsn == InvalidXLogRecPtr && \
	 (active)->free_head == 1)

#define LOG_INDEX_MEM_TBL_NEW_ACTIVE(active, lsn) \
	{ \
		SpinLockAcquire(LOG_INDEX_SNAPSHOT_LOCK); \
		(active)->data.idx_table_id = (++logindex_snapshot->max_idx_table_id); \
		SpinLockRelease(LOG_INDEX_SNAPSHOT_LOCK); \
		(active)->data.max_lsn = InvalidXLogRecPtr; \
		(active)->data.min_lsn = UINT64_MAX; \
		(active)->data.prefix_lsn = ((lsn) >> 32) ; \
		(active)->free_head = 1; \
		LOG_INDEX_MEM_TBL_SET_STATE((active), LOG_INDEX_MEM_TBL_STATE_ACTIVE); \
	}

#define LOG_INDEX_MEM_TBL_FULL(t)  \
	(LOG_INDEX_MEM_TBL_FREE_HEAD(t) == LOG_INDEX_MEM_TBL_SEG_NUM)

#define LOG_INDEX_ORDER_SEG_MASK            0x0FFF
#define LOG_INDEX_ORDER_IDX_MASK            0xF000
#define LOG_INDEX_ORDER_IDX_SHIFT           12
#define LOG_INDEX_SEG_ORDER(idx_order)      ((idx_order) & LOG_INDEX_ORDER_SEG_MASK)
#define LOG_INDEX_ID_ORDER(idx_order)       (((idx_order) & LOG_INDEX_ORDER_IDX_MASK) >> LOG_INDEX_ORDER_IDX_SHIFT)

#define LOG_INDEX_MEM_TBL_ADD_ORDER(t, seg_id, idx) \
	do \
	{  \
		Assert(seg_id < LOG_INDEX_MEM_TBL_SEG_NUM); \
		Assert(idx < (LOG_INDEX_ORDER_IDX_MASK >> LOG_INDEX_ORDER_IDX_SHIFT)); \
		(t)->idx_order[(t)->last_order] = ((seg_id) & LOG_INDEX_ORDER_SEG_MASK) | \
										  (((idx) << LOG_INDEX_ORDER_IDX_SHIFT) & LOG_INDEX_ORDER_IDX_MASK); \
		pg_write_barrier();\
		(t)->last_order++; \
	} \
	while (0)

/* Define macro to calc saved page and offset
 * Be careful that valid table id start from 1
 * */
#define LOG_INDEX_TBL_BLOOM_PAGE_NO(tid) \
	(((tid)-1)*LOG_INDEX_FILE_TBL_BLOOM_SIZE/BLCKSZ)
#define LOG_INDEX_TBL_BLOOM_PAGE_OFFSET(tid) \
	(((tid)-1)*LOG_INDEX_FILE_TBL_BLOOM_SIZE % BLCKSZ)

#define LOG_INDEX_BLOOM_NUM_PER_BLOCK (BLCKSZ/LOG_INDEX_FILE_TBL_BLOOM_SIZE)
#define LOG_INDEX_TABLE_NUM_PER_FILE  (LOG_INDEX_BLOOM_NUM_PER_BLOCK * SLRU_PAGES_PER_SEGMENT)
#define LOG_INDEX_TABLE_CACHE_SIZE    (LOG_INDEX_TABLE_NUM_PER_FILE * sizeof(log_idx_table_data_t))
#define LOG_INDEX_GET_CACHE_TABLE(cache, tid) \
	(((tid) < (cache)->min_idx_table_id || (tid) > (cache)->max_idx_table_id) ? NULL : \
	 (log_idx_table_data_t *)((cache)->data + sizeof(log_idx_table_data_t) * ((tid) - (cache)->min_idx_table_id)))

#define LOG_INDEX_FILE_TABLE_SEGMENT_NO(tid) \
	(((tid)-1) / LOG_INDEX_TABLE_NUM_PER_FILE)

#define LOG_INDEX_FILE_TABLE_SEGMENT_OFFSET(tid) \
	((((tid)-1) % LOG_INDEX_TABLE_NUM_PER_FILE) * sizeof(log_idx_table_data_t))

#define LOG_INDEX_MEM_TBL_STATE_FREE        (0x00)
#define LOG_INDEX_MEM_TBL_STATE_ACTIVE      (0x01)
#define LOG_INDEX_MEM_TBL_STATE_INACTIVE    (0x02)
#define LOG_INDEX_MEM_TBL_STATE_FLUSHED     (0x04)

/*
 * Use coalesced hashing to save mini transaction info
 * See https://en.wikipedia.org/wiki/Coalesced_hashing to get more detail about colesced hashing
 */
#define MINI_TRANSACTION_HASH_SIZE   XLR_MAX_BLOCK_ID
#define MINI_TRANSACTION_TABLE_SIZE  (XLR_MAX_BLOCK_ID + XLR_MAX_BLOCK_ID/5)
#define MINI_TRANSACTION_HASH_PAGE(tag) \
	(tag_hash(tag, sizeof(BufferTag)) % MINI_TRANSACTION_HASH_SIZE)

/*
 * 1. One lwlock for mini_transaction_t
 * 2. The mini transaction table lock size is MINI_TRANSACTION_TABLE_SIZE
 * 3. Each memory table has one lwlock
 * 4. One lwlock for lru
 * 5. The hash lock size is LOG_INDEX_MEM_TBL_HASH_LOCK_NUM
 * 6. One lwlock for logindex meta io
 */
#define LOG_INDEX_LWLOCK_NUM(mem_tbl_size) \
	(1 + \
	 MINI_TRANSACTION_TABLE_SIZE + \
	 mem_tbl_size + \
	 1 + \
	 LOG_INDEX_MEM_TBL_HASH_LOCK_NUM + \
	 1)

#define MINI_TRANSACTION_LOCK_OFFSET                0
#define MINI_TRANSACTION_TABLE_LOCK_OFFSET          (MINI_TRANSACTION_LOCK_OFFSET + 1)
#define LOG_INDEX_MEMTBL_LOCK_OFFSET                (MINI_TRANSACTION_TABLE_LOCK_OFFSET + MINI_TRANSACTION_TABLE_SIZE)
#define LOG_INDEX_BLOOM_LRU_LOCK_OFFSET             (LOG_INDEX_MEMTBL_LOCK_OFFSET + logindex_snapshot->mem_tbl_size)
#define LOG_INDEX_HASH_LOCK_OFFSET                  (LOG_INDEX_BLOOM_LRU_LOCK_OFFSET + 1)
#define LOG_INDEX_IO_LOCK_OFFSET                    (LOG_INDEX_HASH_LOCK_OFFSET + LOG_INDEX_MEM_TBL_HASH_LOCK_NUM)

#define LOG_INDEX_MEM_TBL_ARRAY_INDEX(t)            ((t) - logindex_snapshot->mem_table)

/* Define macro to get lock pointer */
#define LOG_INDEX_SNAPSHOT_LOCK     (&(logindex_snapshot->lock))

#define LOG_INDEX_MEM_TBL_LOCK(t)                   \
	(&(logindex_snapshot->lwlock_array[LOG_INDEX_MEM_TBL_ARRAY_INDEX(t) + LOG_INDEX_MEMTBL_LOCK_OFFSET].lock))

#define LOG_INDEX_BLOOM_LRU_LOCK                    \
	(&(logindex_snapshot->lwlock_array[LOG_INDEX_BLOOM_LRU_LOCK_OFFSET].lock))

#define LOG_INDEX_HASH_LOCK(k)                      \
	(&(logindex_snapshot->lwlock_array[((k) % LOG_INDEX_MEM_TBL_HASH_LOCK_NUM) + LOG_INDEX_HASH_LOCK_OFFSET].lock))

#define MINI_TRANSACTION_LOCK           (&(POLAR_LOGINDEX_WAL_SNAPSHOT->lwlock_array[MINI_TRANSACTION_LOCK_OFFSET].lock))

#define MINI_TRANSACTION_GET_TABLE_LOCK(k)          \
	(&(POLAR_LOGINDEX_WAL_SNAPSHOT->lwlock_array[(k) + MINI_TRANSACTION_TABLE_LOCK_OFFSET].lock))

#define LOG_INDEX_IO_LOCK                     \
	(&(logindex_snapshot->lwlock_array[LOG_INDEX_IO_LOCK_OFFSET].lock))

#define LOG_INDEX_COPY_LSN_INFO(lsn_info, table, item, idx) \
	do \
	{ \
		(lsn_info)->lsn = LOG_INDEX_COMBINE_LSN(table, (item)->suffix_lsn[(idx)]); \
	} \
	while(0)

#define LOG_INDEX_INSERT_LSN_INFO(item, idx, lsn_info) \
	do \
	{ \
		(item)->suffix_lsn[(idx)] = (((lsn_info)->lsn << 32) >> 32); \
	} \
	while(0)

#define LOG_INDEX_COPY_SEG_INFO(stack, i, table, seg, j) \
	do \
	{ \
		(stack)->lsn[(i)] = LOG_INDEX_COMBINE_LSN(table, (seg)->suffix_lsn[(j)]); \
	} \
	while(0)

#define LOG_INDEX_ONLY_IN_MEM (!polar_streaming_xlog_meta)

#define LOG_INDEX_COPY_META(dst_meta)       \
	do \
	{ \
		LWLockAcquire(LOG_INDEX_IO_LOCK, LW_SHARED); \
		memcpy((dst_meta), &(logindex_snapshot->meta), sizeof(log_index_meta_t)); \
		LWLockRelease(LOG_INDEX_IO_LOCK); \
	} \
	while(0)

#define POLAR_ENABLE_FULLPAGE_SNAPSHOT() \
	(polar_enable_fullpage_snapshot && polar_enable_redo_logindex && polar_streaming_xlog_meta)

#define POLAR_PAGETAGS_EQUAL(a,b) \
( \
	RelFileNodeEquals((a).rnode, (b).rnode) && \
	(a).blockNum == (b).blockNum && \
	(a).forkNum == (b).forkNum \
)

typedef enum
{
	ITERATE_STATE_FORWARD,
	ITERATE_STATE_BACKWARD,
	ITERATE_STATE_FINISHED,
	ITERATE_STATE_HOLLOW,
	ITERATE_STATE_CORRUPTED
} log_index_iter_state_t;

/*
 * Use parallel array to save memory usage.
 * If define structure directly, lots of memory will be wasted by structure memory alignment
 */

typedef struct log_item_head_t
{
	log_seg_id_t        head_seg;
	log_seg_id_t        next_item;
	log_seg_id_t        next_seg;
	log_seg_id_t        tail_seg;
	BufferTag           tag;
	uint8               number;
	XLogRecPtr          prev_page_lsn;
	uint32              suffix_lsn[LOG_INDEX_ITEM_HEAD_LSN_NUM];
} log_item_head_t;

typedef struct log_item_seg_t
{
	log_seg_id_t        head_seg;
	log_seg_id_t        next_seg;
	log_seg_id_t        prev_seg;
	uint8               number;
	uint32              suffix_lsn[LOG_INDEX_ITEM_SEG_LSN_NUM];
} log_item_seg_t;

typedef union log_tbl_seg_t
{
	log_item_head_t     item_head;
	log_item_seg_t      item_seg;
	char                padding[LOG_INDEX_TBL_SEG_SIZE];
} log_tbl_seg_t;

typedef struct log_idx_table_data_t
{
	log_idx_table_id_t  idx_table_id;
	XLogRecPtr          min_lsn;
	XLogRecPtr          max_lsn;
	uint32              prefix_lsn;
	pg_crc32            crc;
	uint32              last_order;
	uint16              idx_order[LOG_INDEX_MAX_ORDER_NUM];
	log_seg_id_t        hash[LOG_INDEX_MEM_TBL_HASH_NUM];
	log_tbl_seg_t       segment[LOG_INDEX_MEM_TBL_SEG_NUM];
} log_idx_table_data_t;

typedef struct log_mem_table_t
{
	log_seg_id_t        free_head;
	pg_atomic_uint32    state;
	/* The following data will be saved to file */
	log_idx_table_data_t    data;
} log_mem_table_t;

typedef struct log_file_table_bloom_t
{
	log_idx_table_id_t  idx_table_id;
	XLogRecPtr          min_lsn;
	XLogRecPtr          max_lsn;
	uint32              buf_size;
	pg_crc32            crc;
	uint8               bloom_bytes[FLEXIBLE_ARRAY_MEMBER];
} log_file_table_bloom_t;

typedef struct log_index_file_segment_t
{
	uint64                      segment_no;
	XLogRecPtr                  max_lsn;
	log_idx_table_id_t          max_idx_table_id;
	log_idx_table_id_t          min_idx_table_id;
} log_index_file_segment_t;

typedef struct log_index_meta_t
{
	uint32                                      magic;
	uint32                                      version;
	log_idx_table_id_t                          max_idx_table_id;
	log_index_file_segment_t                    min_segment_info;
	XLogRecPtr                                  start_lsn;
	XLogRecPtr                                  max_lsn;
	uint32                                      crc;
} log_index_meta_t;

typedef struct mini_trans_info_t
{
	BufferTag               tag;
	pg_atomic_uint32        refcount;
	uint32                  next;
} mini_trans_info_t;

typedef struct mini_trans_t
{
	bool                        started;
	XLogRecPtr                  lsn;
	/* Map to info array, if item of info array is in use then corresponding bit is set in occupied */
	uint64                    occupied;
	mini_trans_info_t           info[MINI_TRANSACTION_TABLE_SIZE];
} mini_trans_t;

typedef struct log_table_cache_t
{
	log_idx_table_id_t min_idx_table_id;
	log_idx_table_id_t max_idx_table_id;
	char               data[LOG_INDEX_TABLE_CACHE_SIZE];
} log_table_cache_t;

typedef struct log_index_snapshot_t
{
	int                         type;
	MemoryContext               mem_cxt;
	LWLockMinimallyPadded      *lwlock_array;
	int                         mem_tbl_size;
	SlruCtlData                 bloom_ctl;
	slock_t                     lock;
	char                        dir[NAMEDATALEN];
	char                        meta_file_name[MAXPGPATH];
	XLogRecPtr                  max_lsn;
	pg_atomic_uint32            state;
	uint32                      active_table;
	log_idx_table_id_t          max_idx_table_id;
	log_index_meta_t            meta;
	uint64                      max_allocated_seg_no;
	mini_trans_t                mini_transaction;
	log_mem_table_t             mem_table[FLEXIBLE_ARRAY_MEMBER];
} log_index_snapshot_t;

#define LOG_INDEX_PAGE_STACK_ITEM_SIZE 64
typedef struct log_index_page_lsn_t
{
	uint16                          item_size;
	uint16                          iter_pos;
	XLogRecPtr                      lsn[LOG_INDEX_PAGE_STACK_ITEM_SIZE];
	XLogRecPtr                      prev_lsn;
	struct log_index_page_lsn_t     *next;
} log_index_page_lsn_t;

typedef struct log_index_tbl_stack_lsn_t
{
	XLogRecPtr                          full_page_lsn;
	XLogRecPtr                          prev_page_lsn;
	struct log_index_page_lsn_t         *head;
	struct log_index_tbl_stack_lsn_t    *next;
} log_index_tbl_stack_lsn_t;

typedef struct log_index_page_stack_lsn_t
{
	log_index_tbl_stack_lsn_t    *head;
} log_index_page_stack_lsn_t;

typedef struct log_index_page_iter_data_t
{
	BufferTag                       tag;
	XLogRecPtr                      min_lsn;
	XLogRecPtr                      max_lsn;
	XLogRecPtr                      iter_prev_lsn;
	XLogRecPtr                      iter_max_lsn;
	log_index_page_stack_lsn_t      lsn_stack;
	uint32                          key;
	log_index_iter_state_t          state;
	log_index_lsn_t                 lsn_info;
} log_index_page_iter_data_t;

typedef enum
{
	LOG_INDEX_OPEN_FAILED,
	LOG_INDEX_SEEK_FAILED,
	LOG_INDEX_READ_FAILED,
	LOG_INDEX_WRITE_FAILED,
	LOG_INDEX_FSYNC_FAILED,
	LOG_INDEX_CLOSE_FAILED,
	LOG_INDEX_CRC_FAILED
} log_index_io_err_t;

typedef struct log_index_lsn_iter_data_t
{
	XLogRecPtr              start_lsn;
	log_index_iter_state_t  state;
	log_idx_table_id_t      idx_table_id;
	uint32                  idx;
	BufferTag               tag;
	log_index_lsn_t         lsn_info;
	log_idx_table_id_t      last_search_tid;
} log_index_lsn_iter_data_t;

typedef struct log_index_redo_t
{
	const char *rm_name;
	/* Used by master node to save logindex */
	void (*rm_polar_idx_save)(XLogReaderState *record);
	/* Used by replica node to parse and save logindex */
	bool (*rm_polar_idx_parse)(XLogReaderState *record);
	/* Used to replay XLOG */
	XLogRedoAction(*rm_polar_idx_redo)(XLogReaderState *record, BufferTag *tag, Buffer *buffer);
} log_index_redo_t;

static inline pg_crc32
log_index_calc_crc(unsigned char *data, size_t size)
{
	pg_crc32    crc;

	INIT_CRC32C(crc);

	COMP_CRC32C(crc, data, size);

	FIN_CRC32C(crc);

	return crc;
}

extern log_table_cache_t logindex_table_cache[LOGINDEX_SNAPSHOT_NUM];
extern log_item_head_t *log_index_tbl_find(BufferTag *tag, log_idx_table_data_t *table, uint32 key);
extern void log_index_insert_lsn(logindex_snapshot_t logindex_snapshot, log_index_lsn_t *lsn_info, uint32 key);
extern XLogRecPtr log_index_item_max_lsn(log_idx_table_data_t *table, log_item_head_t *item);
extern pg_crc32 log_index_calc_crc(unsigned char *data, size_t size);

extern bool log_index_write_table(logindex_snapshot_t logindex_snapshot, log_mem_table_t *table);
extern log_idx_table_data_t *log_index_read_table(logindex_snapshot_t logindex_snapshot, log_idx_table_id_t tid);
extern log_file_table_bloom_t *log_index_get_tbl_bloom(logindex_snapshot_t logindex_snapshot, log_idx_table_id_t tid);
extern bool log_index_get_meta(logindex_snapshot_t logindex_snapshot, log_index_meta_t *meta);
extern void log_index_force_save_table(logindex_snapshot_t logindex_snapshot, log_mem_table_t *table);
extern bool log_index_read_table_data(logindex_snapshot_t logindex_snapshot, log_idx_table_data_t *table, log_idx_table_id_t tid, int elevel);

extern XLogRecPtr log_index_get_order_lsn(log_idx_table_data_t *table, uint32 order, log_index_lsn_t *lsn_info);
extern void log_index_master_bg_write(logindex_snapshot_t logindex_snapshot);

extern void polar_log_index_save_lsn(XLogReaderState *state);
extern void polar_log_index_save_block(XLogReaderState *state, uint8 block_id);

extern polar_page_lock_t polar_log_index_mini_trans_key_lock(BufferTag *tag, uint32 key,
															 LWLockMode mode, XLogRecPtr *lsn);
extern XLogRecPtr polar_log_index_mini_trans_key_find(BufferTag *tag, uint32 key);

extern polar_page_lock_t polar_log_index_mini_trans_cond_key_lock(BufferTag *tag, uint32 key,
																  LWLockMode mode, XLogRecPtr *lsn);

extern Buffer polar_log_index_parse(XLogReaderState *state, BufferTag *tag, bool get_cleanup_lock, polar_page_lock_t *page_lock);

extern Buffer polar_log_index_outdate_parse(XLogReaderState *state, BufferTag *tag, bool get_cleanup_lock, polar_page_lock_t *page_lock, bool vm_parse);

extern void polar_log_index_redo_parse(XLogReaderState *state, uint8 block_id);
extern void polar_log_index_cleanup_parse(XLogReaderState *state, uint8 block_id);
extern bool polar_xlog_idx_parse(XLogReaderState *state);
extern void polar_xlog_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_xlog_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_heap2_idx_parse(XLogReaderState *state);
extern void polar_heap2_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_heap2_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_heap_idx_parse(XLogReaderState *state);
extern void polar_heap_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_heap_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_btree_idx_parse(XLogReaderState *state);
extern void polar_btree_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_btree_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_hash_idx_parse(XLogReaderState *state);
extern void polar_hash_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_hash_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);

extern bool polar_gin_idx_parse(XLogReaderState *state);
extern void polar_gin_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_gin_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_gist_idx_parse(XLogReaderState *state);
extern void polar_gist_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_gist_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_seq_idx_parse(XLogReaderState *state);
extern void polar_seq_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_seq_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_spg_idx_parse(XLogReaderState *state);
extern void polar_spg_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_spg_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_brin_idx_parse(XLogReaderState *state);
extern void polar_brin_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_brin_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern bool polar_generic_idx_parse(XLogReaderState *state);
extern void polar_generic_idx_save(XLogReaderState *state);
extern XLogRedoAction polar_generic_idx_redo(XLogReaderState *state,  BufferTag *tag, Buffer *buffer);
extern XLogRecord *polar_log_index_read_xlog(XLogReaderState *state, XLogRecPtr lsn);

#define POLAR_MINI_TRANS_REDO_PARSE(record, block_id, tag, lock, buf) \
	do { \
		POLAR_GET_LOG_TAG((record), (tag), (block_id)); \
		lock = polar_log_index_mini_trans_lock(&(tag), LW_EXCLUSIVE, NULL); \
		buf = polar_log_index_parse((record), &(tag), false, &(lock)); \
	} while (0)

#define POLAR_MINI_TRANS_CLEANUP_PARSE(record, block_id, tag, lock, buf) \
	do { \
		POLAR_GET_LOG_TAG((record), (tag), (block_id)); \
		lock = polar_log_index_mini_trans_lock(&(tag), LW_EXCLUSIVE, NULL); \
		buf = polar_log_index_parse((record), &(tag), true, &(lock)); \
	} while (0)

/*
 * POLAR: Return the newest byte position which all logindex info could be flushed before it.
 */
#define POLAR_LOGINDEX_FLUSHABLE_LSN() \
	(RecoveryInProgress() ? GetXLogReplayRecPtr(NULL) : GetFlushRecPtr())

#endif
