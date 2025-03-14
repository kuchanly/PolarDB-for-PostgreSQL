/*-------------------------------------------------------------------------
 *
 * polar_queue_manager.c
 *      Polar logindex queuen manager
 *
 * Copyright (c) 2019, Alibaba.inc
 *
 * src/backend/access/logindexm/polar_mini_transaction.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * We use queue to keep xlog meta info and clog record.
 * 1. In master mode there exists one send queue which keep xlog meta be sent to replica.
 *  1.1 When backend create XLOG record it reserve space from send queue in atomic way.
 *  1.2 When xlog meta space is reserved from queue backend processes save xlog meta parallelly.
 *  1.3 WAL writer process read xlog data from the queue and save to logindex table.
 *  1.3 WAL sender process read from the queue and send xlog meta to replica
 * 2. In replica mode there exists one recv queue.
 *  2.1 WAL receiver save received xlog meta to receive queue.
 *  2.2 Startup process parse xlog meta from the queue and save to logindex table.
 */
#include "postgres.h"

#include "access/gistxlog.h"
#include "access/hash_xlog.h"
#include "access/heapam_xlog.h"
#include "access/nbtxlog.h"
#include "access/polar_logindex.h"
#include "access/polar_logindex_internal.h"
#include "access/polar_queue_manager.h"
#include "access/spgxlog.h"
#include "access/visibilitymap.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"
#include "postmaster/startup.h"
#include "replication/walreceiver.h"
#include "storage/shmem.h"
#include "utils/guc.h"

typedef uint32 main_data_len_t;
typedef uint8  block_id_t;
#define POLAR_MAIN_DATA_LEN(len) (sizeof(block_id_t) + sizeof(main_data_len_t) + (len))

polar_ringbuf_t  polar_xlog_queue;
static bool      polar_pop_xlog_queue = true;

static bool polar_xlog_recv_queue_check(XLogReaderState *state);

Size
polar_xlog_queue_size(void)
{
	Size size = 0;

	if (!polar_streaming_xlog_meta)
		return size;

	/* xlog queue size */
	size = add_size(size, polar_xlog_queue_buffers * 1024L * 1024L);
	return size;
}

bool
polar_xlog_remove_payload(XLogRecord *record)
{
	RmgrId rmid = record->xl_rmid;
	uint8 info;

	if (rmid == RM_HEAP_ID || rmid == RM_HEAP2_ID || rmid == RM_BTREE_ID || rmid == RM_HASH_ID
			|| rmid == RM_GIN_ID || rmid == RM_GIST_ID || rmid == RM_SEQ_ID
			|| rmid == RM_SPGIST_ID || rmid == RM_BRIN_ID || rmid == RM_GENERIC_ID)
		return true;

	if (rmid != RM_XLOG_ID)
		return false;

	info = record->xl_info & ~XLR_INFO_MASK;

	if (info != XLOG_FPI &&
			info != XLOG_FPI_FOR_HINT &&
			info != XLOG_FPSI)
		return false;

	return true;
}

static inline Size
polar_reserve_main_data_size()
{
	Size size = polar_get_main_data_len();

	return POLAR_MAIN_DATA_LEN(size);
}

static Size
polar_reserve_xlog_data_size(uint8 info)
{
	switch (info)
	{
		/* fullpage_no */
		case XLOG_FPSI:
			return POLAR_MAIN_DATA_LEN(sizeof(uint64));

		default:
			break;
	}

	return 0;
}

static Size
polar_reserve_heap_data_size(uint8 info)
{
	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP_INSERT:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapInsert);

		case XLOG_HEAP_DELETE:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapDelete);

		case XLOG_HEAP_UPDATE:
		case XLOG_HEAP_HOT_UPDATE:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapUpdate);

		case XLOG_HEAP_LOCK:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapLock);

		default:
			break;
	}

	return 0;
}

static Size
polar_reserve_heap2_data_size(uint8 info)
{
	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP2_CLEAN:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapClean);

		case XLOG_HEAP2_FREEZE_PAGE:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapFreezePage);

		case XLOG_HEAP2_CLEANUP_INFO:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapCleanupInfo);

		case XLOG_HEAP2_VISIBLE:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapVisible);

		case XLOG_HEAP2_MULTI_INSERT:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapMultiInsert);

		case XLOG_HEAP2_LOCK_UPDATED:
			return POLAR_MAIN_DATA_LEN(SizeOfHeapLockUpdated);

		case XLOG_HEAP2_REWRITE:
			return POLAR_MAIN_DATA_LEN(sizeof(xl_heap_rewrite_mapping));

		default:
			break;
	}

	return 0;
}


static Size
polar_reserve_gist_data_size(uint8 info)
{
	XLogRecData *rdata = polar_get_main_data_head();

	if (info != XLOG_GIST_PAGE_UPDATE)
		return 0;

	/* Flollowing function gistXLogUpdate to reserve size for gistxlogPageUpdate*/
	if (rdata->next == NULL || rdata->next->len == 0)
		return 0;

	return polar_reserve_main_data_size();
}

static Size
polar_reserve_hash_data_size(uint8 info)
{
	if (info != XLOG_HASH_VACUUM_ONE_PAGE)
		return 0;

	return polar_reserve_main_data_size();
}

static Size
polar_reserve_btree_data_size(uint8 info)
{
	switch (info)
	{
		case XLOG_BTREE_DELETE:
		case XLOG_BTREE_REUSE_PAGE:
			return polar_reserve_main_data_size();

		default:
			break;
	}

	return 0;
}

static Size
polar_reserve_spg_data_size(uint8 info)
{
	if (info != XLOG_SPGIST_VACUUM_REDIRECT)
		return 0;

	return POLAR_MAIN_DATA_LEN(SizeOfSpgxlogVacuumRedirect);
}

static Size
polar_reserve_data_size(XLogRecData *rdata)
{
	XLogRecord *rechdr = (XLogRecord *) rdata->data;
	uint8 info = rechdr->xl_info & ~XLR_INFO_MASK;

	switch (rechdr->xl_rmid)
	{
		case RM_HEAP_ID:
			return polar_reserve_heap_data_size(info);

		case RM_HEAP2_ID:
			return polar_reserve_heap2_data_size(info);

		case RM_GIST_ID:
			return polar_reserve_gist_data_size(info);

		case RM_HASH_ID:
			return polar_reserve_hash_data_size(info);

		case RM_BTREE_ID:
			return polar_reserve_btree_data_size(info);

		case RM_SPGIST_ID:
			return polar_reserve_spg_data_size(info);

		case RM_XLOG_ID:
			return polar_reserve_xlog_data_size(info);

		default:
			break;
	}

	return 0;
}

Size
polar_xlog_reserve_size(XLogRecData *rdata)
{
	XLogRecord *rechdr = (XLogRecord *) rdata->data;
	Size len = polar_xlog_remove_payload(rechdr) ?
			   rdata->len : rechdr->xl_tot_len;
	len += polar_reserve_data_size(rdata);
	return len;
}

void
polar_xlog_queue_init(void)
{
	bool found;
	Size size;
	uint8 *data;

	StaticAssertStmt(POLAR_RINGBUF_MAX_SLOT <= sizeof(uint64) * CHAR_BIT,
					 "POLAR_RINGBUF_MAX_SLOT is larger than 64");

	if (!polar_streaming_xlog_meta)
		return ;

	size = polar_xlog_queue_size();
	data = (uint8 *)ShmemInitStruct("XLOG queue buffer",
									size, &found);

	if (!found)
	{
		MemSet(data, 0, size);
		polar_xlog_queue = polar_ringbuf_init(data, size,
											  LWTRANCHE_POLAR_XLOG_QUEUE,
											  "polar_xlog_queue");
	}
}

#define POLAR_WRITE_MAIN_DATA(dlen, rbuf_pos, offset) \
	do { \
		uint8 block_id = XLR_BLOCK_ID_POLAR_EXTRA; \
		XLogRecData *main_data = polar_get_main_data_head(); \
		uint8 *data = (uint8 *)(main_data->data); \
		main_data_len_t data_len = dlen; \
		if (rbuf_pos < 0 || rbuf_pos >= polar_xlog_queue->size) \
			ereport(PANIC, (errmsg("rbuf_pos=%lu is incorrect for xlog queue, queue size is %lu", \
								   rbuf_pos, polar_xlog_queue->size))); \
		len = polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos, \
									  offset, (uint8 *)(&block_id), sizeof(block_id_t)); \
		len += polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos, \
									   offset+len, (uint8 *)(&data_len), sizeof(data_len)); \
		len += polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos, \
									   offset+len, (uint8 *)(data), data_len); \
	} while (0)

static int
polar_xlog_push_heap_data(size_t rbuf_pos, int offset, uint8 info)
{
	int len = 0;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP_INSERT:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapInsert, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP_DELETE:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapDelete, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP_UPDATE:
		case XLOG_HEAP_HOT_UPDATE:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapUpdate, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP_LOCK:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapLock, rbuf_pos, offset);
			break;
		}

		default:
			break;
	}

	return len;
}

static int
polar_xlog_push_heap2_data(size_t rbuf_pos, int offset, uint8 info)
{
	int len = 0;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP2_CLEAN:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapClean, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP2_FREEZE_PAGE:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapFreezePage, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP2_CLEANUP_INFO:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapCleanupInfo, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP2_VISIBLE:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapVisible, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP2_MULTI_INSERT:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapMultiInsert, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP2_LOCK_UPDATED:
		{
			POLAR_WRITE_MAIN_DATA(SizeOfHeapLockUpdated, rbuf_pos, offset);
			break;
		}

		case XLOG_HEAP2_REWRITE:
		{
			POLAR_WRITE_MAIN_DATA(sizeof(xl_heap_rewrite_mapping), rbuf_pos, offset);
			break;
		}

		default:
			break;
	}

	return len;
}

static int
polar_push_main_data(size_t rbuf_pos, int offset)
{
	int len = 0;
	const main_data_len_t data_len = polar_reserve_main_data_size();
	uint8 block_id = XLR_BLOCK_ID_POLAR_EXTRA;
	XLogRecData *rdata = polar_get_main_data_head();

	if (rbuf_pos >= polar_xlog_queue->size)
		ereport(PANIC, (errmsg("rbuf_pos=%ld is incorrect for xlog queue, xlog_queue_size=%ld",
							   rbuf_pos, polar_xlog_queue->size)));

	len = polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos,
								  offset, (uint8 *)(&block_id), sizeof(block_id));

	len += polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos,
								   offset + len, (uint8 *)(&data_len), sizeof(data_len));

	while (len < data_len)
	{
		len += polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos,
									   offset + len, (uint8 *)(rdata->data), rdata->len);
		rdata = rdata->next;
	}

	return len;
}

static int
polar_xlog_push_gist_data(size_t rbuf_pos, int offset, uint8 info)
{
	XLogRecData *rdata = polar_get_main_data_head();

	if (info != XLOG_GIST_PAGE_UPDATE)
		return 0;

	/* Following function gistXLogUpdate to reserve size for gistxlogPageUpdate */
	if (rdata->next == NULL || rdata->next->len == 0)
		return 0;

	return polar_push_main_data(rbuf_pos, offset);
}

static int
polar_xlog_push_hash_data(size_t rbuf_pos, int offset, uint8 info)
{
	if (info != XLOG_HASH_VACUUM_ONE_PAGE)
		return 0;

	return polar_push_main_data(rbuf_pos, offset);
}

static int
polar_xlog_push_btree_data(size_t rbuf_pos, int offset, uint8 info)
{
	switch (info)
	{
		case XLOG_BTREE_DELETE:
		case XLOG_BTREE_REUSE_PAGE:
			return polar_push_main_data(rbuf_pos, offset);

		default:
			break;
	}

	return 0;
}

static int
polar_xlog_push_spg_data(size_t rbuf_pos, int offset, uint8 info)
{
	int len = 0;

	if (info != XLOG_SPGIST_VACUUM_REDIRECT)
		return len;

	POLAR_WRITE_MAIN_DATA(SizeOfSpgxlogVacuumRedirect, rbuf_pos, offset);

	return len;
}

static int
polar_xlog_push_xlog_data(size_t rbuf_pos, int offset, uint8 info)
{
	int len = 0;

	if (info != XLOG_FPSI)
		return len;

	POLAR_WRITE_MAIN_DATA(sizeof(uint64), rbuf_pos, offset);

	return len;
}

static int
polar_xlog_send_queue_push_data(size_t rbuf_pos, int offset, struct XLogRecData *rdata)
{
	XLogRecord *rechdr = (XLogRecord *) rdata->data;
	uint8 info = rechdr->xl_info & ~XLR_INFO_MASK;
	int len = 0;

	switch (rechdr->xl_rmid)
	{
		case RM_HEAP_ID:
			len = polar_xlog_push_heap_data(rbuf_pos, offset, info);
			break;

		case RM_HEAP2_ID:
			len = polar_xlog_push_heap2_data(rbuf_pos, offset, info);
			break;

		case RM_GIST_ID:
			len = polar_xlog_push_gist_data(rbuf_pos, offset, info);
			break;

		case RM_HASH_ID:
			len = polar_xlog_push_hash_data(rbuf_pos, offset, info);
			break;

		case RM_BTREE_ID:
			len = polar_xlog_push_btree_data(rbuf_pos, offset, info);
			break;

		case RM_SPGIST_ID:
			len = polar_xlog_push_spg_data(rbuf_pos, offset, info);
			break;

		case RM_XLOG_ID:
			len = polar_xlog_push_xlog_data(rbuf_pos, offset, info);
			break;

		default:
			break;
	}

	return len;
}

bool
polar_xlog_send_queue_push(size_t rbuf_pos, XLogRecData *record, int copy_len,
						   XLogRecPtr end_lsn, uint32 xlog_len)
{
	int offset = 0;
	Size data_size = polar_reserve_data_size(record);
	XLogRecData *rdata = record;

	if (!POLAR_XLOG_QUEUE_ENABLE())
		return false;

	if (rbuf_pos >= polar_xlog_queue->size)
		ereport(PANIC, (errmsg("rbuf_pos=%ld is incorrect for xlog queue, xlog_queu_size=%ld",
							   rbuf_pos, polar_xlog_queue->size)));

	offset += polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos,
									  offset, (uint8 *)&end_lsn, sizeof(end_lsn));
	offset += polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos,
									  offset, (uint8 *)&xlog_len, sizeof(xlog_len));

	while ((copy_len - data_size > 0) && rdata)
	{
		/* POLAR: In case, rdata->len is bigger than (copy_len - data_size). */
		ssize_t write_len = Min(copy_len - data_size, rdata->len);
		ssize_t copy_size = polar_ringbuf_pkt_write(polar_xlog_queue, rbuf_pos,
													offset, (uint8 *)(rdata->data), write_len);

		if (copy_size != write_len)
			elog(PANIC, "Failed to write packet to ringbuf, rbuf_pos=%lu, offset=%d, copy_size=%ld, rdata_len=%u",
				 rbuf_pos, offset, copy_size, rdata->len);

		offset += write_len;
		copy_len -= write_len;
		rdata = rdata->next;
	}

	copy_len -= polar_xlog_send_queue_push_data(rbuf_pos, offset, record);

	if (copy_len != 0)
	{
		elog(PANIC, "Failed to push data to send queue, rbuf_pos=%lu, offset=%d, left=%d",
			 rbuf_pos, offset, copy_len);
	}

	polar_ringbuf_set_pkt_flag(polar_xlog_queue, rbuf_pos, POLAR_RINGBUF_PKT_WAL_META | POLAR_RINGBUF_PKT_READY);

	if (polar_enable_debug)
		elog(LOG, "%s lsn=%x/%x", __func__, (uint32)((end_lsn - xlog_len) >> 32), (uint32)(end_lsn - xlog_len));

	return true;
}

/*
 * POLAR: Create XLOG meta and push them into polar_xlog_queue in startup.
 * The caller must make sure that polar_xlog_queue is enable.
 */
void
polar_standby_xlog_send_queue_push(XLogReaderState *xlogreader)
{
	XLogRecPtr  StartPos = xlogreader->ReadRecPtr;
	XLogRecPtr  EndPos = xlogreader->EndRecPtr;
	ssize_t     RbufPos = -1;
	Size        RbufLen = 0;
	XLogRecData rdata =
	{
		.next = NULL,
		.data = xlogreader->readRecordBuf,
		.len  = xlogreader->polar_logindex_meta_size
	};
	static polar_ringbuf_ref_t polar_data_ref = { .slot = -1 };

	/* POLAR: Init send queue reference in standby. */
	if (unlikely(polar_data_ref.slot == -1))
	{
		MemSet(&polar_data_ref, 0, sizeof(polar_ringbuf_ref_t));

		if (!POLAR_XLOG_QUEUE_NEW_REF(&polar_data_ref, true, "xlog_queue_data_keep"))
		{
			POLAR_LOG_XLOG_RECORD_INFO(xlogreader);
			elog(PANIC, "Failed to create send queue reference");
		}

		polar_ringbuf_auto_release_ref(&polar_data_ref);
	}
	else
		polar_xlog_send_queue_keep_data(&polar_data_ref);

	/* POLAR END */
	polar_set_main_data(xlogreader->main_data, xlogreader->main_data_len);
	RbufLen = polar_xlog_reserve_size(&rdata);

	if (!POLAR_XLOG_QUEUE_FREE_SIZE(RbufLen))
		POLAR_XLOG_QUEUE_FREE_UP(RbufLen);

	RbufPos = POLAR_XLOG_QUEUE_RESERVE(RbufLen);
	POLAR_XLOG_QUEUE_SET_PKT_LEN(RbufPos, RbufLen);
	/* POLAR: Make rdata contain the whole xlog. */
	rdata.len = EndPos - StartPos;

	if (polar_xlog_send_queue_push(RbufPos, &rdata, RbufLen, EndPos, EndPos - StartPos))
	{
		WalSndWakeup();

		if (polar_enable_debug)
		{
			ereport(LOG, (errmsg("%s push %X/%X to queue", __func__,
								 (uint32)(EndPos >> 32),
								 (uint32) EndPos)));
		}
	}

	polar_reset_main_data();
}

XLogRecPtr
polar_xlog_send_queue_next_lsn(polar_ringbuf_ref_t *ref, size_t *len)
{
	XLogRecPtr  lsn = InvalidXLogRecPtr;
	uint32      pktlen = 0;

	if (polar_ringbuf_avail(ref) > 0
			&& polar_ringbuf_next_ready_pkt(ref, &pktlen) != POLAR_RINGBUF_PKT_INVALID_TYPE)
	{
		Assert(pktlen > sizeof(XLogRecPtr));
		polar_ringbuf_read_next_pkt(ref, 0, (uint8 *)&lsn, sizeof(XLogRecPtr));

		if (len != NULL)
			*len = pktlen;
	}

	return lsn;
}

ssize_t
polar_xlog_send_queue_pop(polar_ringbuf_ref_t *ref,
						  uint8 *data, size_t size, XLogRecPtr *max_lsn)
{
	uint32 pktlen;
	ssize_t copy_size = 0;
	size_t  free_size = size;
	XLogRecPtr lsn = InvalidXLogRecPtr;

	while (polar_ringbuf_avail(ref) > 0
			&& polar_ringbuf_next_ready_pkt(ref, &pktlen) != POLAR_RINGBUF_PKT_INVALID_TYPE)
	{
		ssize_t len;

		/*
		 * When walreceiver decode data from queue, it read pktlen first and then read packet data,
		 * so free space must be large enough to include uint32 which save pktlen and data which size is
		 * pktlen.
		 */
		if (pktlen + sizeof(uint32) > free_size)
			break;

		polar_ringbuf_read_next_pkt(ref, 0, (uint8 *)&lsn, sizeof(XLogRecPtr));

		/*
		 * Pop XLOG which is already been written out and flushed to disk.
		 * It's unsafe to send XLOG that is not securely down to disk on the master:
		 * if the master crashes and restarts, replica must not hava applied any XLOG
		 * that got lost on the master.
		 * Note: We record end position of XLOG in the queue
		 *
		 * POLAR: Flush lsn is not updated in recovery mode, especially for replica and standby.
		 */
		if (lsn > POLAR_LOGINDEX_FLUSHABLE_LSN())
			break;

		memcpy(data, &pktlen, sizeof(uint32));
		len = sizeof(uint32);
		data += sizeof(uint32);
		len += polar_ringbuf_read_next_pkt(ref, 0, data, pktlen);
		Assert(len == pktlen + sizeof(uint32));

		if (polar_enable_debug)
		{
			uint32 xlog_len;
			memcpy(&xlog_len, data + sizeof(lsn), sizeof(xlog_len));

			elog(LOG, "%s lsn=%x/%x", __func__, (uint32)((lsn - xlog_len) >> 32), (uint32)(lsn - xlog_len));
		}

		data += pktlen;

		polar_ringbuf_update_ref(ref);
		copy_size += len;
		free_size -= len;

		*max_lsn = lsn;
	}

	return copy_size;
}

//TODO: recheck logical
bool
polar_xlog_send_queue_check(polar_ringbuf_ref_t *ref, XLogRecPtr start_point)
{
	uint32 pktlen;
	XLogRecPtr lsn = InvalidXLogRecPtr;
	uint32     lsn_len = 0;
	bool       ret = false;

	if (start_point == InvalidXLogRecPtr)
		return true;

	/*
	 * 1. If it's weak reference, try to promote to strong reference
	 * 2. If reference is evicted, then create a new weak reference and promote new weak reference to strong reference.
	 */
	while (!ref->strong && !polar_ringbuf_get_ref(ref))
	{
		if (!POLAR_XLOG_QUEUE_NEW_REF(ref, false, ref->ref_name))
			elog(PANIC, "Failed to create send queue reference for %s", ref->ref_name);
	}

	do
	{
		XLogRecPtr start_lsn;
		int type;

		if (polar_ringbuf_avail(ref) <= 0
				|| ((type = polar_ringbuf_next_ready_pkt(ref, &pktlen)) == POLAR_RINGBUF_PKT_INVALID_TYPE))
			break;

		if (type == POLAR_RINGBUF_PKT_WAL_META)
		{
			if (polar_ringbuf_read_next_pkt(ref, 0, (uint8 *)&lsn, sizeof(XLogRecPtr))
					!= sizeof(XLogRecPtr))
				elog(PANIC, "Failed to read LSN from ringbuf for ref %s", ref->ref_name);

			if (polar_ringbuf_read_next_pkt(ref, sizeof(XLogRecPtr), (uint8 *)&lsn_len, sizeof(uint32))
					!= sizeof(uint32))
				elog(PANIC, "Failed to read lsn_len from ringbuf for ref %s", ref->ref_name);

			start_lsn = lsn - lsn_len;

			if (start_lsn <= start_point && !ret)
			{
				elog(LOG, "polar_xlog_queue_check succeed for ref %s, start_point = %lx", ref->ref_name, start_point);
				ret = true;
			}

			if (start_lsn >= start_point)
				break;
		}

		polar_ringbuf_update_ref(ref);
	}
	while (true);

	if (!ref->strong)
		polar_ringbuf_clear_ref(ref);

	return ret;
}

static XLogRecord *
polar_xlog_queue_ref_pop(polar_ringbuf_ref_t *ref, XLogReaderState *state, bool decode_payload, char **errormsg)
{
#define COPY_QUEUE_CONTENT(_dst, _size) \
	do {\
		ssize_t len = polar_ringbuf_read_next_pkt(ref, offset, \
												  (uint8*)(_dst), (_size)); \
		if(len != (_size)) \
		{ \
			ereport(PANIC, (errmsg("polar: Failed to read from xlog recv queue for ref %s from offset %ld and size %ld, the return value is %ld", \
								   (ref)->ref_name, offset, (long)(_size), len))); \
		} \
		offset += len; \
	} while (0)

	uint32 pktlen = 0;
	uint32 xlog_len;
	uint32 data_len;
	XLogRecPtr read_rec_ptr, end_rec_ptr;
	XLogRecord *record = NULL;
	ssize_t offset = 0;
	uint8  pkt_type = POLAR_RINGBUF_PKT_INVALID_TYPE;

	if (polar_ringbuf_avail(ref) > 0)
		pkt_type = polar_ringbuf_next_ready_pkt(ref, &pktlen);

	switch (pkt_type)
	{
		case POLAR_RINGBUF_PKT_WAL_META:
		{
			COPY_QUEUE_CONTENT(&end_rec_ptr, sizeof(XLogRecPtr));
			COPY_QUEUE_CONTENT(&xlog_len, sizeof(uint32));
			read_rec_ptr = end_rec_ptr - xlog_len;

			if (read_rec_ptr > state->EndRecPtr && !polar_pop_xlog_queue)
			{
				record = XLogReadRecord(state, InvalidXLogRecPtr, errormsg);

				/* The record in queue is same as the record read from file */
				if (state->ReadRecPtr == read_rec_ptr)
				{
					polar_ringbuf_update_ref(ref);
					polar_pop_xlog_queue = true;
				}
			}
			else
			{
				data_len = pktlen - POLAR_XLOG_HEAD_SIZE;

				if (data_len > state->readRecordBufSize)
					allocate_recordbuf(state, data_len);

				COPY_QUEUE_CONTENT(state->readRecordBuf, data_len);

				polar_ringbuf_update_ref(ref);

				state->readLen = data_len;
				state->EndRecPtr = end_rec_ptr;
				state->ReadRecPtr = read_rec_ptr;
				state->noPayload = true;
				record = (XLogRecord *)state->readRecordBuf;
				polar_xlog_queue_decode(state, record, decode_payload, errormsg);

				if (!polar_pop_xlog_queue)
					elog(LOG, "Polar: switch xlog record source from file to queue");

				polar_pop_xlog_queue = true;
			}

			break;
		}

		case POLAR_RINGBUF_PKT_WAL_STORAGE_BEGIN:
		{
			record = XLogReadRecord(state, InvalidXLogRecPtr, errormsg);
			polar_ringbuf_update_ref(ref);
			polar_pop_xlog_queue = false;
			elog(LOG, "Polar: switch xlog record source from queue to file");
			break;
		}

		case POLAR_RINGBUF_PKT_INVALID_TYPE:
		{
			if (!polar_pop_xlog_queue)
				record = XLogReadRecord(state, InvalidXLogRecPtr, errormsg);

			break;
		}

		default:
			elog(PANIC, "Polar: Invalid xlog queue pkt type %d", pkt_type);
	}

	if (polar_enable_debug)
		elog(LOG, "%s lsn=%x/%x", __func__, (uint32)(state->ReadRecPtr >> 32), (uint32)state->ReadRecPtr);

	return record;
}

static void
polar_reset_blk(DecodedBkpBlock *blk)
{
	blk->in_use = false;
	blk->has_image = false;
	blk->has_data = false;
	blk->apply_image = false;
}

static void
polar_reset_decoder(XLogReaderState *state)
{
	int         block_id;

	state->decoded_record = NULL;

	state->main_data_len = 0;

	for (block_id = 0; block_id <= state->max_block_id; block_id++)
		polar_reset_blk(&state->blocks[block_id]);

	state->max_block_id = -1;
}

static void
polar_heap_save_vm_block(XLogReaderState *state, uint8 block_id, uint8 vm_block_id)
{
	DecodedBkpBlock *blk, *vm_blk;

	Assert(block_id <= XLR_MAX_BLOCK_ID);
	blk = &state->blocks[block_id];
	vm_blk = &state->blocks[vm_block_id];

	Assert(blk->in_use);
	Assert(!vm_blk->in_use);
	polar_reset_blk(vm_blk);

	vm_blk->in_use = true;
	vm_blk->rnode = blk->rnode;
	vm_blk->forknum = VISIBILITYMAP_FORKNUM;
	vm_blk->blkno = HEAPBLK_TO_MAPBLOCK(blk->blkno);

	state->max_block_id = Max(state->max_block_id, vm_block_id);
}

static void
polar_heap_update_save_vm_logindex(XLogReaderState *state, bool hotupdate)
{
	BlockNumber blkno_old, blkno_new;
	xl_heap_update *xlrec = (xl_heap_update *)(state->main_data);

	Assert(state->blocks[0].in_use);
	blkno_new = state->blocks[0].blkno;

	if (state->blocks[1].in_use)
	{
		/* HOT updates are never done across pages */
		Assert(!hotupdate);
		blkno_old = state->blocks[1].blkno;
	}
	else
		blkno_old = blkno_new;

	if (blkno_new != blkno_old)
	{
		if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
			polar_heap_save_vm_block(state, 1, 3);

		if (xlrec->flags & XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED)
			polar_heap_save_vm_block(state, 0, 2);
	}
	else
	{
		if (xlrec->flags & XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED)
			polar_heap_save_vm_block(state, 0, 2);
	}
}

static void
polar_xlog_queue_decode_heap(XLogReaderState *state)
{
	XLogRecord *rechdr = state->decoded_record;
	uint8 info = rechdr->xl_info & ~XLR_INFO_MASK;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP_INSERT:
		{
			xl_heap_insert *xlrec = (xl_heap_insert *)(state->main_data);

			if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
				polar_heap_save_vm_block(state, 0, 1);

			break;
		}

		case XLOG_HEAP_DELETE:
		{
			xl_heap_delete *xlrec = (xl_heap_delete *)(state->main_data);

			if (xlrec->flags & XLH_DELETE_ALL_VISIBLE_CLEARED)
				polar_heap_save_vm_block(state, 0, 1);

			break;
		}

		case XLOG_HEAP_UPDATE:
			polar_heap_update_save_vm_logindex(state, false);
			break;

		case XLOG_HEAP_HOT_UPDATE:
			polar_heap_update_save_vm_logindex(state, true);
			break;

		case XLOG_HEAP_LOCK:
		{
			xl_heap_lock *xlrec = (xl_heap_lock *)(state->main_data);

			if (xlrec->flags & XLH_LOCK_ALL_FROZEN_CLEARED)
				polar_heap_save_vm_block(state, 0, 1);

			break;
		}
	}
}

static void
polar_xlog_queue_decode_heap2(XLogReaderState *state)
{
	XLogRecord *rechdr = state->decoded_record;
	uint8 info = rechdr->xl_info & ~XLR_INFO_MASK;

	switch (info & XLOG_HEAP_OPMASK)
	{
		case XLOG_HEAP2_MULTI_INSERT:
		{
			xl_heap_multi_insert *xlrec = (xl_heap_multi_insert *)(state->main_data);

			if (xlrec->flags & XLH_INSERT_ALL_VISIBLE_CLEARED)
				polar_heap_save_vm_block(state, 0, 1);

			break;
		}

		case XLOG_HEAP2_LOCK_UPDATED:
		{
			xl_heap_lock_updated *xlrec = (xl_heap_lock_updated *)(state->main_data);

			if (xlrec->flags & XLH_LOCK_ALL_FROZEN_CLEARED)
				polar_heap_save_vm_block(state, 0, 1);

			break;
		}

		default:
			break;
	}
}

void
polar_xlog_decode_data(XLogReaderState *state)
{
	XLogRecord *rechdr = state->decoded_record;

	switch (rechdr->xl_rmid)
	{
		case RM_HEAP_ID:
			polar_xlog_queue_decode_heap(state);
			break;

		case RM_HEAP2_ID:
			polar_xlog_queue_decode_heap2(state);
			break;

		default:
			break;
	}
}

bool
polar_xlog_queue_decode(XLogReaderState *state, XLogRecord *record, bool decode_payload, char **errormsg)
{
#define COPY_CONTENT(_dst, _size) \
	do {\
		if((state->readLen - offset) < (_size)) \
		{ \
			POLAR_LOG_XLOG_RECORD_INFO(state); \
			ereport(PANIC, (errmsg("polar: Failed to read from xlog send queue from offset %ld and size %ld, the remaining size is %ld", \
								   offset, (long)(_size), state->readLen - offset))); \
		} \
		memcpy(_dst, ptr + offset, _size); \
		offset += (_size); \
	} while (0)

	uint8  block_id;
	ssize_t offset = 0;
	RelFileNode     *rnode = NULL;
	char *ptr = (char *)record;

	polar_reset_decoder(state);
	offset += SizeOfXLogRecord;

	if (polar_xlog_remove_payload(record))
	{
		state->decoded_record = record;

		while (offset < state->readLen)
		{
			DecodedBkpBlock *blk;

			COPY_CONTENT(&block_id, sizeof(block_id));

			if (block_id == XLR_BLOCK_ID_DATA_SHORT)
			{
				uint8 data_len;
				COPY_CONTENT(&data_len, sizeof(uint8));
				continue;
			}
			else if (block_id == XLR_BLOCK_ID_DATA_LONG)
			{
				uint32 data_len;
				COPY_CONTENT(&data_len, sizeof(uint32));
				continue;
			}
			else if (block_id == XLR_BLOCK_ID_ORIGIN)
			{
				RepOriginId id;
				COPY_CONTENT(&id, sizeof(RepOriginId));
				continue;
			}
			else if (block_id == XLR_BLOCK_ID_POLAR_EXTRA)
			{
				main_data_len_t data_len;

				COPY_CONTENT(&data_len, sizeof(data_len));
				state->main_data_len = data_len;

				if (state->main_data_len > 0)
				{
					if (!state->main_data || state->main_data_len > state->main_data_bufsz)
					{
						if (state->main_data)
							pfree(state->main_data);

						state->main_data_bufsz = MAXALIGN(Max(state->main_data_len,
															  BLCKSZ / 2));
						state->main_data = palloc(state->main_data_bufsz);
					}

					memcpy(state->main_data, ptr + offset, data_len);
					offset += data_len;

					if (polar_enable_debug)
					{
						ereport(LOG, (errmsg("rmid=%d info=%d main_data_len=%d",
											 record->xl_rmid, record->xl_info,
											 state->main_data_len)));
					}
				}

				continue;
			}

			if (block_id <= state->max_block_id)
			{
				report_invalid_record(state,
									  "polar: xlog queue out-of-order block_id %u and %X/%X",
									  block_id,
									  (uint32)(state->ReadRecPtr >> 32),
									  (uint32)(state->ReadRecPtr));
				goto err;
			}

			state->max_block_id = block_id;

			blk = &state->blocks[block_id];
			blk->in_use = true;
			blk->apply_image = false;

			COPY_CONTENT(&blk->flags, sizeof(uint8));
			blk->forknum = blk->flags & BKPBLOCK_FORK_MASK;
			blk->has_image = ((blk->flags & BKPBLOCK_HAS_IMAGE) != 0);
			blk->has_data = ((blk->flags & BKPBLOCK_HAS_DATA) != 0);

			COPY_CONTENT(&blk->data_len, sizeof(uint16));

			/* cross-check that the HAS_DATA flag is set if data_len>0 */
			if (blk->has_data && blk->data_len == 0)
			{
				report_invalid_record(state,
									  "polar: xlog queue BKPBLOCK_HAS_DATA set, but no data included at block_id %u and %X/%X",
									  block_id,
									  (uint32)(state->ReadRecPtr >> 32),
									  (uint32)(state->ReadRecPtr));
				goto err;
			}

			if (!blk->has_data && blk->data_len != 0)
			{
				report_invalid_record(state,
									  "polar: xlog queue BKPBLOCK_HAS_DATA not set, but data len is %d included at block_id %u and %X/%X",
									  blk->data_len, block_id,
									  (uint32)(state->ReadRecPtr >> 32),
									  (uint32)(state->ReadRecPtr));
				goto err;
			}

			if (blk->has_image)
			{
				COPY_CONTENT(&blk->bimg_len, sizeof(uint16));
				COPY_CONTENT(&blk->hole_offset, sizeof(uint16));
				COPY_CONTENT(&blk->bimg_info, sizeof(uint8));

				blk->apply_image = ((blk->bimg_info & BKPIMAGE_APPLY) != 0);

				if (blk->bimg_info & BKPIMAGE_IS_COMPRESSED)
				{
					if (blk->bimg_info & BKPIMAGE_HAS_HOLE)
						COPY_CONTENT(&blk->hole_length, sizeof(uint16));
					else
						blk->hole_length = 0;
				}
				else
					blk->hole_length = BLCKSZ - blk->bimg_len;

				/*
				 * cross-check that hole_offset > 0, hole_length > 0 and
				 * bimg_len < BLCKSZ if the HAS_HOLE flag is set.
				 */
				if ((blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
						(blk->hole_offset == 0 ||
						 blk->hole_length == 0 ||
						 blk->bimg_len == BLCKSZ))
				{

					report_invalid_record(state,
										  "polar: xlog queue BKPIMAGE_HAS_HOLE set ,but hole offset %u length %u block image length %u at block_id %u and %X/%X",
										  (unsigned int) blk->hole_offset,
										  (unsigned int) blk->hole_length,
										  (unsigned int) blk->bimg_len,
										  block_id,
										  (uint32)(state->ReadRecPtr >> 32),
										  (uint32)(state->ReadRecPtr));
					goto err;
				}

				/*
				 * cross-check that hole_offset == 0 and hole_length == 0 if
				 * the HAS_HOLE flag is not set.
				 */

				if (!(blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
						(blk->hole_offset != 0 || blk->hole_length != 0))
				{
					report_invalid_record(state,
										  "polar: xlog queue BKPIMAGE_HAS_HOLE not set, but hole offset %u length %u at block_id %u and %X/%X",
										  (unsigned int) blk->hole_offset,
										  (unsigned int) blk->hole_length,
										  block_id,
										  (uint32)(state->ReadRecPtr >> 32),
										  (uint32)(state->ReadRecPtr));
					goto err;
				}

				/*
				 * cross-check that bimg_len < BLCKSZ if the IS_COMPRESSED
				 * flag is set.
				 */
				if ((blk->bimg_info & BKPIMAGE_IS_COMPRESSED) &&
						blk->bimg_len == BLCKSZ)
				{
					report_invalid_record(state,
										  "polar: xlog queue BKPIMAGE_IS_COMPRESSED set , but block image length %u at block_id %u and %X/%X",
										  (unsigned int) blk->bimg_len,
										  block_id,
										  (uint32)(state->ReadRecPtr >> 32),
										  (uint32)(state->ReadRecPtr));
					goto err;
				}

				/*
				 * cross-check that bimg_len = BLCKSZ if neither HAS_HOLE nor
				 * IS_COMPRESSED flag is set.
				 */
				if (!(blk->bimg_info & BKPIMAGE_HAS_HOLE) &&
						!(blk->bimg_info & BKPIMAGE_IS_COMPRESSED) &&
						blk->bimg_len != BLCKSZ)
				{
					report_invalid_record(state,
										  "polar: xlog queue neither BKPIMAGE_HAS_HOLE nor BKPIMAGE_IS_COMPRESSED set, but block image length is %u at block_id %u and %X/%X",
										  (unsigned int) blk->data_len,
										  block_id,
										  (uint32)(state->ReadRecPtr >> 32),
										  (uint32)(state->ReadRecPtr));
					goto err;
				}
			}

			if (!(blk->flags & BKPBLOCK_SAME_REL))
			{
				COPY_CONTENT(&blk->rnode, sizeof(RelFileNode));
				rnode = &blk->rnode;
			}
			else
			{
				if (rnode == NULL)
				{
					report_invalid_record(state,
										  "polar: xlog queue BKPBLOCK_SAME_REL set but no previous rel at block_id %u and %X/%X",
										  block_id,
										  (uint32)(state->ReadRecPtr >> 32),
										  (uint32)(state->ReadRecPtr));
					goto err;
				}

				blk->rnode = *rnode;
			}

			COPY_CONTENT(&blk->blkno, sizeof(BlockNumber));
		}

		polar_xlog_decode_data(state);
	}
	else if (decode_payload)
		return DecodeXLogRecord(state, record, errormsg);

	return true;

err:

	if (state->errormsg_buf[0] != '\0')
		*errormsg = state->errormsg_buf;

	return false;
}

void
polar_xlog_send_queue_save(polar_ringbuf_ref_t *ref)
{
	uint32 pktlen;
	XLogRecord *record;
	uint8 pkt_type;
	char *errormsg = NULL;
	static XLogReaderState *state = NULL;

	if (state == NULL)
		state = XLogReaderAllocate(wal_segment_size, NULL, NULL);

	while (polar_ringbuf_avail(ref) > 0
			&& (pkt_type = polar_ringbuf_next_ready_pkt(ref, &pktlen)) != POLAR_RINGBUF_PKT_INVALID_TYPE)
	{
		XLogRecPtr lsn = InvalidXLogRecPtr;

		if (pkt_type != POLAR_RINGBUF_PKT_WAL_META)
		{
			elog(PANIC, "For xlog send queue there should be only WAL META, but we got %d from ref %s",
				 pkt_type, ref->ref_name);
		}

		polar_ringbuf_read_next_pkt(ref, 0, (uint8 *)&lsn, sizeof(XLogRecPtr));

		/* Master save logindex when xlog is flushed */
		if (lsn > GetFlushRecPtr())
			break;

		record = polar_xlog_queue_ref_pop(ref, state, false, &errormsg);

		if (record == NULL)
		{
			if (errormsg)
				elog(WARNING, "Got errormsg when pop xlog queue: %s", errormsg);

			POLAR_LOG_XLOG_RECORD_INFO(state);
			elog(PANIC, "Failed to pop record from xlog queue from ref %s", ref->ref_name);
		}

		if (polar_xlog_remove_payload(record))
			polar_log_index_save_lsn(state);
	}
}

void
polar_xlog_send_queue_keep_data(polar_ringbuf_ref_t *ref)
{
	polar_ringbuf_t rbuf = ref->rbuf;
	ssize_t keep_size = rbuf->size * 2 / 3;
	bool updated = false;

	while (polar_ringbuf_avail(ref) > keep_size)
	{
		polar_ringbuf_update_ref(ref);
		updated = true;
	}

	if (updated)
		polar_ringbuf_update_keep_data(rbuf);
}

void
polar_xlog_recv_queue_push_storage_begin(polar_interrupt_callback callback)
{
	ssize_t idx;

	while (polar_ringbuf_free_size(polar_xlog_queue) < POLAR_RINGBUF_PKTHDRSIZE)
		polar_ringbuf_free_up(polar_xlog_queue, POLAR_RINGBUF_PKTHDRSIZE, callback);

	idx = polar_ringbuf_pkt_reserve(polar_xlog_queue, POLAR_RINGBUF_PKTHDRSIZE);

	if (idx < 0)
		ereport(PANIC, (errmsg("Failed to reserve space from xlog recv queue for storage packet")));

	polar_ringbuf_set_pkt_length(polar_xlog_queue, idx, 0);

	polar_ringbuf_set_pkt_flag(polar_xlog_queue, idx, POLAR_RINGBUF_PKT_WAL_STORAGE_BEGIN | POLAR_RINGBUF_PKT_READY);

}

bool
polar_xlog_recv_queue_push(char *buf, size_t len, polar_interrupt_callback callback)
{
	ssize_t idx;
	ssize_t copy_len = 0;

	if (!polar_streaming_xlog_meta)
		return false;

	do
	{
		uint32 pktlen;
		uint32 write_len;

		memcpy(&pktlen, buf, sizeof(uint32));
		buf += sizeof(uint32);
		copy_len += sizeof(uint32);

		while (polar_ringbuf_free_size(polar_xlog_queue) < POLAR_RINGBUF_PKT_SIZE(pktlen))
			polar_ringbuf_free_up(polar_xlog_queue, POLAR_RINGBUF_PKT_SIZE(pktlen), callback);

		idx = polar_ringbuf_pkt_reserve(polar_xlog_queue, POLAR_RINGBUF_PKT_SIZE(pktlen));

		if (idx < 0 || idx >= polar_xlog_queue->size)
		{
			ereport(PANIC, (errmsg("Failed to reserve space from xlog recv queue, idx=%ld, queue size=%ld",
								   idx, polar_xlog_queue->size)));
		}

		polar_ringbuf_set_pkt_length(polar_xlog_queue, idx, pktlen);

		write_len = polar_ringbuf_pkt_write(polar_xlog_queue, idx, 0, (uint8 *)buf, pktlen);

		if (write_len != pktlen)
		{
			ereport(PANIC, (errmsg("Failed to copy xlog recv queue, idx=%ld, queue size=%ld, pktlen=%d and write_len=%d",
								   idx, polar_xlog_queue->size, pktlen, write_len)));
		}

		polar_ringbuf_set_pkt_flag(polar_xlog_queue, idx, POLAR_RINGBUF_PKT_WAL_META | POLAR_RINGBUF_PKT_READY);

		if (polar_enable_debug)
		{
			XLogRecPtr lsn;
			uint32  xlog_len;

			memcpy(&lsn, buf, sizeof(lsn));
			memcpy(&xlog_len, buf + sizeof(lsn), sizeof(xlog_len));

			if (polar_enable_debug)
				elog(LOG, "%s lsn=%x/%x", __func__, (uint32)((lsn - xlog_len) >> 32), (uint32)(lsn - xlog_len));

		}

		buf += write_len;
		copy_len += write_len;

	}
	while (copy_len < len);

	if (copy_len != len)
	{
		ereport(PANIC, (errmsg("Failed to copy xlog recv queue, idx=%ld, queue size=%ld, len=%ld and copy_len=%ld",
							   idx, polar_xlog_queue->size, len, copy_len)));
	}

	return true;
}

static bool
polar_xlog_recv_queue_check(XLogReaderState *state)
{
	if (state->currRecPtr == state->ReadRecPtr)
		return true;

	if (state->currRecPtr % state->wal_segment_size == 0 &&
			(state->ReadRecPtr - state->currRecPtr) == SizeOfXLogLongPHD)
		return true;

	if (state->currRecPtr % XLOG_BLCKSZ == 0 &&
			(state->ReadRecPtr - state->currRecPtr) == SizeOfXLogShortPHD)
		return true;

	return false;
}

/*
 * POLAR: this function is only used for polar worker pop record from queue
 */
XLogRecord *
polar_fullpage_bgworker_xlog_recv_queue_pop(log_index_snapshot_t *logindex_snapshot, XLogReaderState *state, char **errormsg)
{
	XLogRecord *record;
	static TimestampTz last_load_logindex_time = 0;
	static polar_ringbuf_ref_t ref = { .slot = -1} ;

	if (unlikely(ref.slot == -1))
	{
		if (!polar_ringbuf_new_ref(polar_xlog_queue, true, &ref, "fullpage_recv_queue_ref"))
		{
			POLAR_LOG_XLOG_RECORD_INFO(state);
			ereport(PANIC, (errmsg("Failed to get ref from xlog recv queue")));
		}

		polar_ringbuf_auto_release_ref(&ref);
	}

	*errormsg = NULL;
	state->errormsg_buf[0] = '\0';

	/*
	 * POLAR: when read from wal file instead of queue maybe very slow.
	 * we use replay_lsn for polar worker catch up with starup replay
	 * when replay fullpage wal, because polar worker don't need to
	 * replay wal lsn less then replay_lsn, the future page fullpage
	 * snapshot wal lsn must be bigger then replay_lsn
	 */
	if (!polar_pop_xlog_queue)
	{
		XLogRecPtr last_replay_end_ptr = GetXLogReplayRecPtr(NULL);

		if (state->EndRecPtr < last_replay_end_ptr)
			state->EndRecPtr = last_replay_end_ptr;
	}

	state->currRecPtr = state->EndRecPtr;

	/*
	 * POLAR: maybe fullpage logindex is ahead of queue record lsn, so we
	 * need skip these outdate records, because we load logindex from storage
	 * which master has flush active table
	 */
	while ((record = polar_xlog_queue_ref_pop(&ref, state, true, errormsg)) == NULL ||
			(state->ReadRecPtr <= logindex_snapshot->max_lsn))
	{
		if (record == NULL)
		{
			TimestampTz now = GetCurrentTimestamp();

			/* Handle interrupts of startup process */
			HandleStartupProcInterrupts();

			if (*errormsg != NULL)
				break;

			/*
			 * We load logindex from storage in following cases:
			 * 1. read from file instead of queue
			 * 2. wal receiver hang or lost for a long time(1s?)
			 * 3. periodly load logindex from storage(1s)
			 */
			if (!polar_pop_xlog_queue ||
					TimestampDifferenceExceeds(
						polar_get_walrcv_last_msg_receipt_time(), now, 1000) ||
					TimestampDifferenceExceeds(
						last_load_logindex_time, now, 1000))
			{
				last_load_logindex_time = now;
				polar_load_logindex_snapshot_from_storage(logindex_snapshot, InvalidXLogRecPtr, false);
			}

			/*
			 * POLAR: when read from wal file, try to update EndRecPtr, maybe
			 * read file very slow
			 */
			if (!polar_pop_xlog_queue)
			{
				XLogRecPtr last_replay_end_ptr = GetXLogReplayRecPtr(NULL);

				if (state->EndRecPtr < last_replay_end_ptr)
					state->EndRecPtr = last_replay_end_ptr;

				state->currRecPtr = state->EndRecPtr;
			}

			polar_fullpage_bgworker_wait_primary_xlog_message(state);
		}
	}

	return record;
}

XLogRecord *
polar_xlog_recv_queue_pop(XLogReaderState *state, XLogRecPtr RecPtr, char **errormsg)
{
	XLogRecord *record;
	bool streaming_reply_sent = false;
	static polar_ringbuf_ref_t ref = { .slot = -1} ;

	if (unlikely(ref.slot == -1))
	{
		if (!polar_ringbuf_new_ref(polar_xlog_queue, true, &ref, "recv_queue_ref"))
		{
			POLAR_LOG_XLOG_RECORD_INFO(state);
			ereport(PANIC, (errmsg("Failed to get ref from xlog recv queue")));
		}

		polar_ringbuf_auto_release_ref(&ref);
	}

	*errormsg = NULL;
	state->errormsg_buf[0] = '\0';

	if (RecPtr == InvalidXLogRecPtr)
		RecPtr = state->EndRecPtr;

	state->currRecPtr = RecPtr;

	while ((record = polar_xlog_queue_ref_pop(&ref, state, true, errormsg)) == NULL ||
			(state->ReadRecPtr < state->currRecPtr))
	{
		/*
		 * Since we have replayed everything we have received so
		 * far and are about to start waiting for more WAL, let's
		 * tell the upstream server our replay location now so
		 * that pg_stat_replication doesn't show stale
		 * information.
		 */

		if (!streaming_reply_sent)
		{
			WalRcvForceReply();
			streaming_reply_sent = true;
		}

		if (record == NULL)
		{
			/* Handle interrupts of startup process */
			HandleStartupProcInterrupts();

			if (!WalRcvStreaming())
			{
				if (CheckForStandbyTrigger())
					break;
				else
				{
					/* POLAR: In shared storage, we need wal receiver to communicate with rw node, so keep it up. */
					polar_keep_wal_receiver_up(state);
				}
			}

			if (*errormsg != NULL)
				break;

			polar_wait_primary_xlog_message(state);
		}
	}

	if (record != NULL)
	{
		if (!polar_xlog_recv_queue_check(state))
		{
			elog(WARNING, "xlog queue pop unmatched record: currRecPtr=%lX, ReadRecPtr=%lX, check whether it's CHECKPOINT_SHUTDOWN",
				 state->currRecPtr, state->ReadRecPtr);
		}

		polar_update_receipt_time();
		polar_set_read_and_end_rec_ptr(state->ReadRecPtr, state->EndRecPtr);

		/* POLAR: In shared storage, we need wal receiver to communicate with rw node, so keep it up. */
		if (!polar_pop_xlog_queue)
			polar_keep_wal_receiver_up(state);
	}

	return record;
}
