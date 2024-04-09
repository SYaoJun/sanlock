/*
 * Copyright 2010-2011 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>

#include "sanlock_internal.h"
#include "diskio.h"
#include "ondisk.h"
#include "direct.h"
#include "log.h"
#include "lockspace.h"
#include "delta_lease.h"
#include "paxos_lease.h"
#include "resource.h"
#include "timeouts.h"

uint32_t crc32c(uint32_t crc, uint8_t *data, size_t length);
int get_rand(int a, int b);

/*
 * BK_DEBUG_SIZE:  size of buffer to hold ballot debug info,
 *                 this can't be larger than LOG_STR_LEN 512
 * BK_STR_SIZE:    the max length of a dblock string for one host
 * BK_DEBUG_COUNT: the max number of hosts for which we'll copy
 *                 dblock info
 *
 * BK_DEBUG_COUNT * BK_STR_SIZE + extra debug text that comes before
 * the dblock info needs to be less than BK_DEBUG_SIZE.
 * Be very careful about increasing BK_DEBUG_COUNT because the use
 * of strncat depends on it.
 */
#define BK_DEBUG_SIZE 512
#define BK_DEBUG_COUNT 4
#define BK_STR_SIZE 80

static uint32_t roundup_power_of_two(uint32_t val)
{
	val--;
	val |= val >> 1;
	val |= val >> 2;
	val |= val >> 4;
	val |= val >> 8;
	val |= val >> 16;
	val++;
	return val;
}

uint32_t leader_checksum(struct leader_record *lr)
{
	return crc32c((uint32_t)~1, (uint8_t *)lr, LEADER_CHECKSUM_LEN);
}

uint32_t dblock_checksum(struct paxos_dblock *pd)
{
	return crc32c((uint32_t)~1, (uint8_t *)pd, DBLOCK_CHECKSUM_LEN);
}

int paxos_lease_request_read(struct task *task, struct token *token,
			     struct request_record *rr)
{
	struct request_record rr_end;
	int rv;

	/* 1 = request record is second sector */

	rv = read_sectors(&token->disks[0], token->sector_size, 1, 1, (char *)&rr_end,
			  sizeof(struct request_record),
			  task, token->io_timeout, "request");
	if (rv < 0)
		return rv;

	request_record_in(&rr_end, rr);

	return SANLK_OK;
}

int paxos_lease_request_write(struct task *task, struct token *token,
			      struct request_record *rr)
{
	struct request_record rr_end;
	int rv;

	request_record_out(rr, &rr_end);

	rv = write_sector(&token->disks[0], token->sector_size, 1, (char *)&rr_end,
			  sizeof(struct request_record),
			  task, token->io_timeout, "request");
	if (rv < 0)
		return rv;

	return SANLK_OK;
}

static int write_dblock(struct task *task,
		        struct token *token,
			struct sync_disk *disk,
			uint64_t host_id,
			struct paxos_dblock *pd);

int paxos_erase_dblock(struct task *task,
		       struct token *token,
		       uint64_t host_id)
{
	struct paxos_dblock dblock_end;
	int num_disks = token->r.num_disks;
	int num_writes = 0;
	int d, rv, error = -1;

	memset(&dblock_end, 0, sizeof(struct paxos_dblock));

	for (d = 0; d < num_disks; d++) {
		rv = write_dblock(task, token, &token->disks[d], host_id, &dblock_end);
		if (rv < 0) {
			error = rv;
			continue;
		}
		num_writes++;
	}

	if (!majority_disks(num_disks, num_writes))
		return error;
	return SANLK_OK;
}

/*
 * Write a combined dblock and mblock.  This is an odd case that doesn't fit
 * well with the way the code has been written.  It's used when we want to
 * convert sh to ex, which requires acquiring the lease owner, but we don't
 * want to clobber our SHARED mblock by writing a plain dblock in the process
 * in case there's a problem with the acquiring, we don't want to loose our
 * shared mode lease.
 *
 * NB. this assumes the only mblock flag we want is MBLOCK_SHARED and that
 * the generation we want is token->host_generation.  This is currently
 * the case, but could change in the future.
 */

static int write_dblock_mblock_sh(struct task *task,
			          struct token *token,
			          struct sync_disk *disk,
			          uint64_t host_id,
			          struct paxos_dblock *pd)
{
	struct paxos_dblock pd_end;
	struct mode_block mb;
	struct mode_block mb_end;
	char *iobuf, **p_iobuf;
	uint64_t offset;
	uint32_t checksum;
	int iobuf_len, rv, sector_size;

	memset(&mb, 0, sizeof(mb));
	mb.flags = MBLOCK_SHARED;
	mb.generation = token->host_generation;

	sector_size = token->sector_size;

	iobuf_len = sector_size;
	if (!iobuf_len)
		return -EINVAL;

	p_iobuf = &iobuf;

	rv = posix_memalign((void *)p_iobuf, getpagesize(), iobuf_len);
	if (rv)
		return -ENOMEM;

	offset = disk->offset + ((2 + host_id - 1) * sector_size);

	paxos_dblock_out(pd, &pd_end);

	/*
	 * N.B. must compute checksum after the data has been byte swapped.
	 */
	checksum = dblock_checksum(&pd_end);
	pd->checksum = checksum;
	pd_end.checksum = cpu_to_le32(checksum);

	mode_block_out(&mb, &mb_end);

	memcpy(iobuf, (char *)&pd_end, sizeof(struct paxos_dblock));
	memcpy(iobuf + MBLOCK_OFFSET, (char *)&mb_end, sizeof(struct mode_block));

	rv = write_iobuf(disk->fd, offset, iobuf, iobuf_len, task, token->io_timeout, NULL);

	if (rv < 0) {
		log_errot(token, "write_dblock_mblock_sh host_id %llu gen %llu rv %d",
			  (unsigned long long)host_id,
			  (unsigned long long)token->host_generation,
			  rv);
	}

	if (rv != SANLK_AIO_TIMEOUT)
		free(iobuf);
	return rv;
}

static int write_dblock(struct task *task,
		        struct token *token,
			struct sync_disk *disk,
			uint64_t host_id,
			struct paxos_dblock *pd)
{
	struct paxos_dblock pd_end;
	uint32_t checksum;
	int rv;

	if (token->flags & T_WRITE_DBLOCK_MBLOCK_SH) {
		/* special case to preserve our SH mode block within the dblock */
		return write_dblock_mblock_sh(task, token, disk, host_id, pd);
	}

	/* 1 leader block + 1 request block;
	   host_id N is block offset N-1 */

	paxos_dblock_out(pd, &pd_end);

	/*
	 * N.B. must compute checksum after the data has been byte swapped.
	 */
	checksum = dblock_checksum(&pd_end);
	pd->checksum = checksum;
	pd_end.checksum = cpu_to_le32(checksum);

	rv = write_sector(disk, token->sector_size, 2 + host_id - 1, (char *)&pd_end, sizeof(struct paxos_dblock),
			  task, token->io_timeout, "dblock");
	return rv;
}

static int write_leader(struct task *task,
		        struct token *token,
			struct sync_disk *disk,
			struct leader_record *lr)
{
	struct leader_record lr_end;
	uint32_t checksum;
	int rv;

	leader_record_out(lr, &lr_end);

	/*
	 * N.B. must compute checksum after the data has been byte swapped.
	 */
	checksum = leader_checksum(&lr_end);
	lr->checksum = checksum;
	lr_end.checksum = cpu_to_le32(checksum);

	rv = write_sector(disk, token->sector_size, 0, (char *)&lr_end, sizeof(struct leader_record),
			  task, token->io_timeout, "leader");
	return rv;
}

/*
 * NB. this should not be used to write the leader record, it is meant only
 * for manually clobbering the disk to corrupt it for testing, or to manually
 * repair it after it's corrupted.
 */
/*
这不应该用于写leader记录，它只意味着手动破坏磁盘以进行测试，或者在磁盘损坏后手动修复它。
*/

int paxos_lease_leader_clobber(struct task *task,
			       struct token *token,
			       struct leader_record *leader,
			       const char *caller)
{
	struct leader_record lr_end;
	uint32_t checksum;
	int rv;

	leader_record_out(leader, &lr_end);

	/*
	 * N.B. must compute checksum after the data has been byte swapped.
	 */
	checksum = leader_checksum(&lr_end);
	leader->checksum = checksum;
	lr_end.checksum = cpu_to_le32(checksum);

	rv = write_sector(&token->disks[0], token->sector_size, 0, (char *)&lr_end, sizeof(struct leader_record),
			  task, token->io_timeout, caller);
	return rv;
}

static int read_dblock(struct task *task,
		       struct token *token,
		       struct sync_disk *disk,
		       uint64_t host_id,
		       struct paxos_dblock *pd)
{
	struct paxos_dblock pd_end;
	int rv;

	/* 1 leader block + 1 request block; host_id N is block offset N-1 */

	rv = read_sectors(disk, token->sector_size, 2 + host_id - 1, 1, (char *)&pd_end, sizeof(struct paxos_dblock),
			  task, token->io_timeout, "dblock");

	paxos_dblock_in(&pd_end, pd);

	return rv;
}

#if 0
static int read_dblocks(struct task *task,
			struct sync_disk *disk,
			struct paxos_dblock *pds,
			int pds_count)
{
	struct paxos_dblock pd_end;
	char *data;
	int data_len, rv, i;

	data_len = pds_count * sector_size;

	data = malloc(data_len);
	if (!data) {
		log_error("read_dblocks malloc %d %s", data_len, disk->path);
		rv = -ENOMEM;
		goto out;
	}

	/* 2 = 1 leader block + 1 request block */

	rv = read_sectors(disk, token->sector_size, 2, pds_count, data, data_len,
			  task, "dblocks");
	if (rv < 0)
		goto out_free;

	/* copy the first N bytes from each sector, where N is size of
	   paxos_dblock */

	for (i = 0; i < pds_count; i++) {
		memcpy(&pd_end, data + (i * sector_size),
		       sizeof(struct paxos_dblock));

		paxos_dblock_in(&pd_end, &pd);

		memcpy(&pds[i], &pd, sizeof(struct paxos_dblock));
	}

	rv = 0;
 out_free:
	free(data);
 out:
	return rv;
}
#endif

static int read_leader(struct task *task,
		       struct token *token,
		       struct sync_disk *disk,
		       struct leader_record *lr,
		       uint32_t *checksum)
{
	struct leader_record lr_end;
	int rv;

	if (!token->sector_size) {
		log_errot(token, "paxos read_leader with zero sector_size");
		return -EINVAL;
	}

	/* 0 = leader record is first sector */

	rv = read_sectors(disk, token->sector_size, 0, 1, (char *)&lr_end, sizeof(struct leader_record),
			  task, token->io_timeout, "leader");

	/* N.B. checksum is computed while the data is in ondisk format. */
	*checksum = leader_checksum(&lr_end);

	leader_record_in(&lr_end, lr);

	return rv;
}

static int verify_dblock(struct token *token, struct paxos_dblock *pd, uint32_t checksum)
{
	if (!pd->checksum && !pd->mbal && !pd->bal && !pd->inp && !pd->lver)
		return SANLK_OK;

	if (pd->checksum != checksum) {
		log_errot(token, "verify_dblock wrong checksum %x %x",
			  pd->checksum, checksum);
		return SANLK_DBLOCK_CHECKSUM;
	}

	return SANLK_OK;
}
/*
有可能我们从另一个具有我们自己 inp 值的主机中选择了一个 bk_max，
并且我们最终会提交我们自己从另一个主机的 dblock 中复制的 inp 值。
---------------------------------------------------------------
通过什么方式判断leader是free的，是通过时间戳吗？？？leader.timestamp == LEASE_FREE
*/
/*
 * It's possible that we pick a bk_max from another host which has our own
 * inp values in it, and we can end up committing our own inp values, copied
 * from another host's dblock:
 *
 * host2 leader free
 * host2 phase1 mbal 14002
 * host2 writes dblock[1] mbal 14002
 * host2 reads  no higher mbal
 * host2 choose own inp 2,1
 * host2 phase2 mbal 14002 bal 14002 inp 2,1
 * host2 writes dblock[1] bal 14002 inp 2,1
 *                                           host1 leader free
 *                                           host1 phase1 mbal 20001
 *                                           host1 writes dblock[0] mbal 20001
 *                                           host1 reads  no higher mbal
 *                                           host1 choose dblock[1] bal 14002 inp 2,1
 *                                           host1 phase2 mbal 20001 bal 20001 inp 2,1
 *                                           host1 writes dblock[0] bal 20001 inp 2,1
 * host2 reads  dblock[0] mbal 20001 > 14002
 *              abort2, retry
 * host2 leader free
 * host2 phase1 mbal 16002
 * host2 writes dblock[1] mbal 16002
 * host2 reads  dblock[0] mbal 20001 > 16002
 *       abort1 retry
 * host2 leader free
 * host2 phase1 mbal 18002
 * host2 writes dblock[1] mbal 18002
 * host2 reads  dblock[0] mbal 20001 > 18002
 *       abort1 retry
 * host2 leader free
 * host2 phase1 mbal 20002
 * host2 writes dblock[1] mbal 20002
 * host2 reads  no higher mbal
 * host2 choose dblock[0] bal 20001 inp 2,1
 *                                           host1 reads  dblock[1] mbal 20002 > 20001
 *                                                 abort2 retry
 * host2 phase2 mbal 20002 bal 20002 inp 2,1
 * host2 writes dblock[1] bal 20002 inp 2,1
 * host2 reads  no higher mbal
 * host2 commit inp 2,1
 * host2 success
 *                                           host1 leader owner 2,1
 *                                           host1 fail
 */

static int run_ballot(struct task *task, struct token *token, uint32_t flags,
		      int num_hosts, uint64_t next_lver, uint64_t our_mbal,
		      struct paxos_dblock *dblock_out)
{
	char bk_debug[BK_DEBUG_SIZE];
	char bk_str[BK_STR_SIZE];
	int bk_debug_count;
	struct paxos_dblock dblock;
	struct paxos_dblock bk_in;
	struct paxos_dblock bk_max;
	struct paxos_dblock *bk_end;
	struct paxos_dblock *bk;
	struct sync_disk *disk;
	char *iobuf[SANLK_MAX_DISKS];
	char **p_iobuf[SANLK_MAX_DISKS];
	uint32_t checksum;
	int num_disks = token->r.num_disks;
	int num_writes, num_reads;
	int sector_size = token->sector_size;
	int sector_count;
	int iobuf_len;
	int phase2 = 0;
	int d, q, rv = 0;
	int q_max = -1;
	int error;

	sector_count = roundup_power_of_two(num_hosts + 2);

	iobuf_len = sector_count * sector_size;

	if (!iobuf_len)
		return -EINVAL;

	for (d = 0; d < num_disks; d++) {
		p_iobuf[d] = &iobuf[d];

		rv = posix_memalign((void *)p_iobuf[d], getpagesize(), iobuf_len);
		if (rv)
			return rv;
	}


	/*
	 * phase 1
	 *
	 * "For each disk d, it tries first to write dblock[p] to disk[d][p]
	 * and then to read disk[d][q] for all other processors q.  It aborts
	 * the ballot if, for any d and q, it finds disk[d][q].mbal >
	 * dblock[p].mbal. The phase completes when p has written and read a
	 * majority of the disks, without reading any block whose mbal
	 * component is greater than dblock[p].mbal."
	 */

	log_token(token, "ballot %llu phase1 write mbal %llu",
		  (unsigned long long)next_lver,
		  (unsigned long long)our_mbal);

	memset(&dblock, 0, sizeof(struct paxos_dblock));
	dblock.mbal = our_mbal;
	dblock.lver = next_lver;
	dblock.checksum = 0; /* set after paxos_dblock_out */

	memset(&bk_max, 0, sizeof(struct paxos_dblock));

	num_writes = 0;

	for (d = 0; d < num_disks; d++) {
		/* acquire io: write 1 */
		rv = write_dblock(task, token, &token->disks[d], token->host_id, &dblock);
		if (rv < 0)
			continue;
		num_writes++;
	}

	if (!majority_disks(num_disks, num_writes)) {
		log_errot(token, "ballot %llu dblock write error %d",
			  (unsigned long long)next_lver, rv);
		error = SANLK_DBLOCK_WRITE;
		goto out;
	}

	memset(bk_debug, 0, sizeof(bk_debug));
	bk_debug_count = 0;

	num_reads = 0;

	for (d = 0; d < num_disks; d++) {
		disk = &token->disks[d];

		if (!iobuf[d])
			continue;
		memset(iobuf[d], 0, iobuf_len);

		/* acquire io: read 2 */
		rv = read_iobuf(disk->fd, disk->offset, iobuf[d], iobuf_len, task, token->io_timeout, NULL);
		if (rv == SANLK_AIO_TIMEOUT)
			iobuf[d] = NULL;
		if (rv < 0)
			continue;
		num_reads++;

		for (q = 0; q < num_hosts; q++) {
			bk_end = (struct paxos_dblock *)(iobuf[d] + ((2 + q)*sector_size));

			checksum = dblock_checksum(bk_end);

			paxos_dblock_in(bk_end, &bk_in);
			bk = &bk_in;

			if (bk_in.mbal && ((flags & PAXOS_ACQUIRE_DEBUG_ALL) || (bk_in.lver >= dblock.lver))) {
				if (bk_debug_count >= BK_DEBUG_COUNT) {
					log_token(token, "ballot %llu phase1 read %s",
						  (unsigned long long)next_lver, bk_debug);
					memset(bk_debug, 0, sizeof(bk_debug));
					bk_debug_count = 0;
				}

				memset(bk_str, 0, sizeof(bk_str));
				snprintf(bk_str, BK_STR_SIZE, "%d:%llu:%llu:%llu:%llu:%llu:%llu:%x,", q,
					 (unsigned long long)bk_in.mbal,
					 (unsigned long long)bk_in.bal,
					 (unsigned long long)bk_in.inp,
					 (unsigned long long)bk_in.inp2,
					 (unsigned long long)bk_in.inp3,
					 (unsigned long long)bk_in.lver,
					 bk_in.flags);
				bk_str[BK_STR_SIZE-1] = '\0';
				strncat(bk_debug, bk_str, BK_STR_SIZE-1);
				bk_debug_count++;
			}

			rv = verify_dblock(token, bk, checksum);
			if (rv < 0)
				continue;

			check_mode_block(token, next_lver, q, (char *)bk_end);

			if (bk->lver < dblock.lver)
				continue;

			if (bk->lver > dblock.lver) {
				log_warnt(token, "ballot %llu abort1 larger lver in bk[%d] %llu:%llu:%llu:%llu:%llu:%llu "
					  "our dblock %llu:%llu:%llu:%llu:%llu:%llu",
					  (unsigned long long)next_lver, q,
					  (unsigned long long)bk->mbal,
					  (unsigned long long)bk->bal,
					  (unsigned long long)bk->inp,
					  (unsigned long long)bk->inp2,
					  (unsigned long long)bk->inp3,
					  (unsigned long long)bk->lver,
					  (unsigned long long)dblock.mbal,
					  (unsigned long long)dblock.bal,
					  (unsigned long long)dblock.inp,
					  (unsigned long long)dblock.inp2,
					  (unsigned long long)dblock.inp3,
					  (unsigned long long)dblock.lver);

				log_token(token, "ballot %llu phase1 read %s",
					  (unsigned long long)next_lver, bk_debug);

				error = SANLK_DBLOCK_LVER;
				goto out;
			}

			/* see "It aborts the ballot" in comment above */

			if (bk->mbal > dblock.mbal) {
				log_warnt(token, "ballot %llu abort1 larger mbal in bk[%d] %llu:%llu:%llu:%llu:%llu:%llu "
					  "our dblock %llu:%llu:%llu:%llu:%llu:%llu",
					  (unsigned long long)next_lver, q,
					  (unsigned long long)bk->mbal,
					  (unsigned long long)bk->bal,
					  (unsigned long long)bk->inp,
					  (unsigned long long)bk->inp2,
					  (unsigned long long)bk->inp3,
					  (unsigned long long)bk->lver,
					  (unsigned long long)dblock.mbal,
					  (unsigned long long)dblock.bal,
					  (unsigned long long)dblock.inp,
					  (unsigned long long)dblock.inp2,
					  (unsigned long long)dblock.inp3,
					  (unsigned long long)dblock.lver);

				log_token(token, "ballot %llu phase1 read %s",
					  (unsigned long long)next_lver, bk_debug);

				error = SANLK_DBLOCK_MBAL;
				goto out;
			}

			/* see choosing inp for phase 2 in comment below */

			if (!bk->inp)
				continue;

			if (!bk->bal) {
				log_errot(token, "ballot %llu zero bal inp[%d] %llu",
					  (unsigned long long)next_lver, q,
					  (unsigned long long)bk->inp);
				continue;
			}

			if (bk->bal > bk_max.bal) {
				bk_max = *bk;
				q_max = q;
			}
		}
	}

	log_token(token, "ballot %llu phase1 read %s",
		  (unsigned long long)next_lver, bk_debug);

	if (!majority_disks(num_disks, num_reads)) {
		log_errot(token, "ballot %llu dblock read error %d",
			  (unsigned long long)next_lver, rv);
		error = SANLK_DBLOCK_READ;
		goto out;
	}


	/*
	 * "When it completes phase 1, p chooses a new value of dblock[p].inp,
	 * sets dblock[p].bal to dblock[p].mbal (its current ballot number),
	 * and begins phase 2."
	 *
	 * "We now describe how processor p chooses the value of dblock[p].inp
	 * that it tries to commit in phase 2. Let blocksSeen be the set
	 * consisting of dblock[p] and all the records disk[d][q] read by p in
	 * phase 1. Let nonInitBlks be the subset of blocksSeen consisting of
	 * those records whose inp field is not NotAnInput.  If nonInitBlks is
	 * empty, then p sets dblock[p].inp to its own input value input[p].
	 * Otherwise, it sets dblock[p].inp to bk.inp for some record bk in
	 * nonInitBlks having the largest value of bk.bal."
	 */

	if (bk_max.inp) {
		/* lver and mbal are already set */
		dblock.inp = bk_max.inp;
		dblock.inp2 = bk_max.inp2;
		dblock.inp3 = bk_max.inp3;
	} else {
		// 如果是空集，就直接设置我们传入的值
		/* lver and mbal are already set */
		dblock.inp = token->host_id;
		dblock.inp2 = token->host_generation;
		dblock.inp3 = monotime();
	}
	dblock.bal = dblock.mbal;
	dblock.checksum = 0; /* set after paxos_dblock_out */

	if (bk_max.inp) {
		log_token(token, "ballot %llu choose bk_max[%d] lver %llu mbal %llu bal %llu inp %llu %llu %llu",
			  (unsigned long long)next_lver, q_max,
			  (unsigned long long)bk_max.lver,
			  (unsigned long long)bk_max.mbal,
			  (unsigned long long)bk_max.bal,
			  (unsigned long long)bk_max.inp,
			  (unsigned long long)bk_max.inp2,
			  (unsigned long long)bk_max.inp3);
	}


	/*
	 * phase 2
	 *
	 * Same description as phase 1, same sequence of writes/reads.
	 */

	phase2 = 1;

	log_token(token, "ballot %llu phase2 write bal %llu inp %llu %llu %llu q_max %d",
		  (unsigned long long)dblock.lver,
		  (unsigned long long)dblock.bal,
		  (unsigned long long)dblock.inp,
		  (unsigned long long)dblock.inp2,
		  (unsigned long long)dblock.inp3,
		  q_max);

	num_writes = 0;

	for (d = 0; d < num_disks; d++) {
		/* acquire io: write 2 */
		rv = write_dblock(task, token, &token->disks[d], token->host_id, &dblock);
		if (rv < 0)
			continue;
		num_writes++;
	}

	if (!majority_disks(num_disks, num_writes)) {
		log_errot(token, "ballot %llu our dblock write2 error %d",
			  (unsigned long long)next_lver, rv);
		error = SANLK_DBLOCK_WRITE;
		goto out;
	}

	memset(bk_debug, 0, sizeof(bk_debug));
	bk_debug_count = 0;

	num_reads = 0;

	for (d = 0; d < num_disks; d++) {
		disk = &token->disks[d];

		if (!iobuf[d])
			continue;
		memset(iobuf[d], 0, iobuf_len);

		/* acquire io: read 3 */
		rv = read_iobuf(disk->fd, disk->offset, iobuf[d], iobuf_len, task, token->io_timeout, NULL);
		if (rv == SANLK_AIO_TIMEOUT)
			iobuf[d] = NULL;
		if (rv < 0)
			continue;
		num_reads++;

		for (q = 0; q < num_hosts; q++) {
			bk_end = (struct paxos_dblock *)(iobuf[d] + ((2 + q)*sector_size));

			checksum = dblock_checksum(bk_end);

			paxos_dblock_in(bk_end, &bk_in);
			bk = &bk_in;

			if (bk->mbal && ((flags & PAXOS_ACQUIRE_DEBUG_ALL) || (bk->lver >= dblock.lver))) {
				if (bk_debug_count >= BK_DEBUG_COUNT) {
					log_token(token, "ballot %llu phase2 read %s",
						  (unsigned long long)next_lver, bk_debug);
					memset(bk_debug, 0, sizeof(bk_debug));
					bk_debug_count = 0;
				}

				memset(bk_str, 0, sizeof(bk_str));
				snprintf(bk_str, BK_STR_SIZE, "%d:%llu:%llu:%llu:%llu:%llu:%llu:%x,", q,
					 (unsigned long long)bk->mbal,
					 (unsigned long long)bk->bal,
					 (unsigned long long)bk->inp,
					 (unsigned long long)bk->inp2,
					 (unsigned long long)bk->inp3,
					 (unsigned long long)bk->lver,
					 bk->flags);
				bk_str[BK_STR_SIZE-1] = '\0';
				strncat(bk_debug, bk_str, BK_STR_SIZE-1);
				bk_debug_count++;
			}

			rv = verify_dblock(token, bk, checksum);
			if (rv < 0)
				continue;

			if (bk->lver < dblock.lver)
				continue;

			if (bk->lver > dblock.lver) {
				/*
				 * This happens when we choose another host's bk, that host
				 * acquires the lease itself, releases it, and reacquires it
				 * with a new lver, all before we get here, at which point
				 * we see the larger lver.  I believe case this would always
				 * also be caught the the bk->mbal > dblock.mbal condition
				 * below.
				 */
				log_warnt(token, "ballot %llu abort2 larger lver in bk[%d] %llu:%llu:%llu:%llu:%llu:%llu "
					  "our dblock %llu:%llu:%llu:%llu:%llu:%llu",
					  (unsigned long long)next_lver, q,
					  (unsigned long long)bk->mbal,
					  (unsigned long long)bk->bal,
					  (unsigned long long)bk->inp,
					  (unsigned long long)bk->inp2,
					  (unsigned long long)bk->inp3,
					  (unsigned long long)bk->lver,
					  (unsigned long long)dblock.mbal,
					  (unsigned long long)dblock.bal,
					  (unsigned long long)dblock.inp,
					  (unsigned long long)dblock.inp2,
					  (unsigned long long)dblock.inp3,
					  (unsigned long long)dblock.lver);

				log_token(token, "ballot %llu phase2 read %s",
					  (unsigned long long)next_lver, bk_debug);

				error = SANLK_DBLOCK_LVER;
				goto out;
			}

			/* see "It aborts the ballot" in comment above */

			if (bk->mbal > dblock.mbal) {
				log_warnt(token, "ballot %llu abort2 larger mbal in bk[%d] %llu:%llu:%llu:%llu:%llu:%llu "
					  "our dblock %llu:%llu:%llu:%llu:%llu:%llu",
					  (unsigned long long)next_lver, q,
					  (unsigned long long)bk->mbal,
					  (unsigned long long)bk->bal,
					  (unsigned long long)bk->inp,
					  (unsigned long long)bk->inp2,
					  (unsigned long long)bk->inp3,
					  (unsigned long long)bk->lver,
					  (unsigned long long)dblock.mbal,
					  (unsigned long long)dblock.bal,
					  (unsigned long long)dblock.inp,
					  (unsigned long long)dblock.inp2,
					  (unsigned long long)dblock.inp3,
					  (unsigned long long)dblock.lver);

				log_token(token, "ballot %llu phase2 read %s",
					  (unsigned long long)next_lver, bk_debug);

				error = SANLK_DBLOCK_MBAL;
				goto out;
			}
		}
	}

	log_token(token, "ballot %llu phase2 read %s",
		  (unsigned long long)next_lver, bk_debug);

	if (!majority_disks(num_disks, num_reads)) {
		log_errot(token, "ballot %llu dblock read2 error %d",
			  (unsigned long long)next_lver, rv);
		error = SANLK_DBLOCK_READ;
		goto out;
	}

	/* "When it completes phase 2, p has committed dblock[p].inp." */

	memcpy(dblock_out, &dblock, sizeof(struct paxos_dblock));
	error = SANLK_OK;
 out:
	for (d = 0; d < num_disks; d++) {
		/* don't free iobufs that have timed out */
		if (!iobuf[d])
			continue;
		free(iobuf[d]);
	}

	if (phase2 && (error < 0) &&
	    ((error == SANLK_DBLOCK_READ) || (error == SANLK_DBLOCK_WRITE))) {
		/*
		 * After phase2 we might "win" the ballot even if we don't complete it
		 * because another host could could pick and commit our dblock values.
		 * If we abort the acquire, but are granted the lease, this would leave
		 * us owning the lease on disk.  With this flag, the release path will
		 * try to ensure we are not and do not become the lease owner.
		 */
		/*
		在第二阶段之后，即使我们没有完成选举，我们也可能会“获胜”，因为另一个主机可能会选择并提交我们的 dblock 值。如果我们中止获取，但被授予租约，那么这将使我们在磁盘上拥有租约。
		通过此标志，释放路径将尝试确保我们不是租约的所有者，也不会成为租约的所有者。
		-------------
		这个标识应该会在后面会用到
		*/
		token->flags |= T_RETRACT_PAXOS;

		log_errot(token, "ballot %llu retract error %d",
			  (unsigned long long)next_lver, error);
	}

	memcpy(dblock_out, &dblock, sizeof(struct paxos_dblock));
	return error;
}

static void log_leader_error(int result,
			     struct token *token,
			     struct sync_disk *disk,
			     struct leader_record *lr,
			     const char *caller)
{
	log_errot(token, "leader1 %s error %d sn %.48s rn %.48s",
		  caller ? caller : "unknown",
		  result,
		  token->r.lockspace_name,
		  token->r.name);

	log_errot(token, "leader2 path %s offset %llu fd %d",
		  disk->path,
		  (unsigned long long)disk->offset,
		  disk->fd);

	log_errot(token, "leader3 m %x v %x ss %u nh %llu mh %llu oi %llu og %llu lv %llu",
		  lr->magic,
		  lr->version,
		  lr->sector_size,
		  (unsigned long long)lr->num_hosts,
		  (unsigned long long)lr->max_hosts,
		  (unsigned long long)lr->owner_id,
		  (unsigned long long)lr->owner_generation,
		  (unsigned long long)lr->lver);

	log_errot(token, "leader4 sn %.48s rn %.48s ts %llu cs %x",
		  lr->space_name,
		  lr->resource_name,
		  (unsigned long long)lr->timestamp,
		  lr->checksum);

	log_errot(token, "leader5 wi %llu wg %llu wt %llu",
		  (unsigned long long)lr->write_id,
		  (unsigned long long)lr->write_generation,
		  (unsigned long long)lr->write_timestamp);
}

static int _verify_leader(struct token *token,
			 struct sync_disk *disk,
			 struct leader_record *lr,
			 uint32_t checksum,
			 const char *caller,
			 int print_error)
{
	struct leader_record leader_end;
	struct leader_record leader_rr;
	int result, rv;

	if (lr->magic == PAXOS_DISK_CLEAR)
		return SANLK_LEADER_MAGIC;

	if (lr->magic != PAXOS_DISK_MAGIC) {
		result = SANLK_LEADER_MAGIC;
		goto fail;
	}

	if ((lr->version & 0xFFFF0000) != PAXOS_DISK_VERSION_MAJOR) {
		result = SANLK_LEADER_VERSION;
		goto fail;
	}

	if (strncmp(lr->space_name, token->r.lockspace_name, NAME_ID_SIZE)) {
		result = SANLK_LEADER_LOCKSPACE;
		goto fail;
	}

	if (strncmp(lr->resource_name, token->r.name, NAME_ID_SIZE)) {
		result = SANLK_LEADER_RESOURCE;
		goto fail;
	}

	if (lr->num_hosts < token->host_id) {
		result = SANLK_LEADER_NUMHOSTS;
		goto fail;
	}

	if (lr->checksum != checksum) {
		result = SANLK_LEADER_CHECKSUM;
		goto fail;
	}

	return SANLK_OK;

 fail:
	if (!print_error)
		return result;

	switch (result) {
	case SANLK_LEADER_MAGIC:
		log_errot(token, "verify_leader wrong magic %x %s",
			  lr->magic, disk->path);
		break;
	case SANLK_LEADER_VERSION:
		log_errot(token, "verify_leader wrong version %x %s",
			  lr->version, disk->path);
		break;
	case SANLK_LEADER_LOCKSPACE:
		log_errot(token, "verify_leader wrong space name %.48s %.48s %s",
			  lr->space_name, token->r.lockspace_name, disk->path);
		break;
	case SANLK_LEADER_RESOURCE:
		log_errot(token, "verify_leader wrong resource name %.48s %.48s %s",
			  lr->resource_name, token->r.name, disk->path);
		break;
	case SANLK_LEADER_NUMHOSTS:
		log_errot(token, "verify_leader num_hosts too small %llu %llu %s",
			  (unsigned long long)lr->num_hosts,
			  (unsigned long long)token->host_id, disk->path);
		break;
	case SANLK_LEADER_CHECKSUM:
		log_errot(token, "verify_leader wrong checksum %x %x %s",
			  lr->checksum, checksum, disk->path);
		break;
	};

	log_leader_error(result, token, disk, lr, caller);

	memset(&leader_end, 0, sizeof(struct leader_record));

	rv = read_sectors(disk, token->sector_size, 0, 1, (char *)&leader_end,
			  sizeof(struct leader_record),
			  NULL, 1, "paxos_verify");

	leader_record_in(&leader_end, &leader_rr);

	log_leader_error(rv, token, disk, &leader_rr, "paxos_verify");

	return result;
}

static int verify_leader(struct token *token,
			 struct sync_disk *disk,
			 struct leader_record *lr,
			 uint32_t checksum,
			 const char *caller)
{
	return _verify_leader(token, disk, lr, checksum, caller, 1);
}

static int verify_leader_no_error(struct token *token,
			 struct sync_disk *disk,
			 struct leader_record *lr,
			 uint32_t checksum,
			 const char *caller)
{
	return _verify_leader(token, disk, lr, checksum, caller, 0);
}

int paxos_verify_leader(struct token *token,
			 struct sync_disk *disk,
			 struct leader_record *lr,
			 uint32_t checksum,
			 const char *caller)
{
	return verify_leader(token, disk, lr, checksum, caller);
}

static int leaders_match(struct leader_record *a, struct leader_record *b)
{
	if (!memcmp(a, b, LEADER_COMPARE_LEN))
		return 1;
	return 0;
}

/* read the lockspace name and resource name given the disk location */

int paxos_read_resource(struct task *task,
			struct token *token,
			struct sanlk_resource *res)
{
	struct leader_record leader;
	uint32_t checksum;
	int align_size;
	int tmp_sector_size = 0;
	int rv;

	memset(&leader, 0, sizeof(struct leader_record));

	/*
	 * We don't know the sector size, so we don't know if we should read
	 * 512 or 4k, but it doesn't matter since the leader record is all that
	 * we need.  It's probably better to read 4k on a 512 disk than to read 512
	 * on a 4k disk, so always do a 4k read.
	 */
	if (!token->sector_size) {
		token->sector_size = 4096;
		token->align_size = sector_size_to_align_size_old(4096);
		tmp_sector_size = 1;
	}

	rv = read_leader(task, token, &token->disks[0], &leader, &checksum);
	if (rv < 0)
		return rv;

	if (!res->lockspace_name[0])
		memcpy(token->r.lockspace_name, leader.space_name, NAME_ID_SIZE);

	if (!res->name[0])
		memcpy(token->r.name, leader.resource_name, NAME_ID_SIZE);

	if (token->flags & T_CHECK_EXISTS) {
		if (leader.magic != PAXOS_DISK_MAGIC)
			rv = SANLK_LEADER_MAGIC;
		else
			rv = SANLK_OK;
	} else {
		rv = verify_leader_no_error(token, &token->disks[0], &leader, checksum, "read_resource");
	}

	if (rv == SANLK_OK) {
		memcpy(res->lockspace_name, leader.space_name, NAME_ID_SIZE);
		memcpy(res->name, leader.resource_name, NAME_ID_SIZE);
		res->lver = leader.lver;

		if ((leader.sector_size == 512) || (leader.sector_size == 4096)) {
			align_size = leader_align_size_from_flag(leader.flags);
			if (!align_size)
				align_size = sector_size_to_align_size_old(leader.sector_size);

			token->sector_size = leader.sector_size;
			token->align_size = align_size;

			/* The flags set by the user may be wrong. */
			sanlk_res_sector_flags_clear(&res->flags);
			sanlk_res_align_flags_clear(&res->flags);

			res->flags |= sanlk_res_sector_size_to_flag(leader.sector_size);
			res->flags |= sanlk_res_align_size_to_flag(align_size);
		} else if (tmp_sector_size) {
			/* we don't know the correct value, so don't set any */
			/* FIXME: add a note about when this can happen */
			token->sector_size = 0;
			token->align_size = 0;
		}
	}

	return rv;
}

int paxos_read_buf(struct task *task,
		   struct token *token,
		   char **buf_out)
{
	char *iobuf, **p_iobuf;
	struct sync_disk *disk = &token->disks[0];
	int rv, iobuf_len;

	if (!token->sector_size || !token->align_size) {
		log_errot(token, "paxos_read_buf with sector_size %d align_size %d",
			  token->sector_size, token->align_size);
		return -EINVAL;
	}

	iobuf_len = token->align_size;
	if (iobuf_len < 0)
		return iobuf_len;

	p_iobuf = &iobuf;

	rv = posix_memalign((void *)p_iobuf, getpagesize(), iobuf_len);
	if (rv)
		return rv;

	memset(iobuf, 0, iobuf_len);

	rv = read_iobuf(disk->fd, disk->offset, iobuf, iobuf_len, task, token->io_timeout, NULL);

	*buf_out = iobuf;

	return rv;
}

static int _leader_read_one(struct task *task,
			    struct token *token,
			    struct leader_record *leader_ret,
			    const char *caller)
{
	struct leader_record leader;
	uint32_t checksum;
	int rv;

	memset(&leader, 0, sizeof(struct leader_record));

	rv = read_leader(task, token, &token->disks[0], &leader, &checksum);
	if (rv < 0)
		return rv;

	rv = verify_leader(token, &token->disks[0], &leader, checksum, caller);

	/* copy what we read even if verify finds a problem */

	memcpy(leader_ret, &leader, sizeof(struct leader_record));
	return rv;
}

/* TODO: completely untested */

static int _leader_read_num(struct task *task,
			    struct token *token,
			    struct leader_record *leader_ret,
			    const char *caller)
{
	struct leader_record leader;
	struct leader_record *leaders;
	uint32_t checksum;
	int *leader_reps;
	int leaders_len, leader_reps_len;
	int num_reads;
	int num_disks = token->r.num_disks;
	int rv = 0, d, i, found;
	int error;

	leaders_len = num_disks * sizeof(struct leader_record);
	leader_reps_len = num_disks * sizeof(int);

	leaders = malloc(leaders_len);
	if (!leaders)
		return -ENOMEM;

	leader_reps = malloc(leader_reps_len);
	if (!leader_reps) {
		free(leaders);
		return -ENOMEM;
	}

	/*
	 * find a leader block that's consistent on the majority of disks,
	 * so we can use as the basis for the new leader
	 */

	memset(&leader, 0, sizeof(struct leader_record));
	memset(leaders, 0, leaders_len);
	memset(leader_reps, 0, leader_reps_len);

	num_reads = 0;

	for (d = 0; d < num_disks; d++) {
		rv = read_leader(task, token, &token->disks[d], &leaders[d], &checksum);
		if (rv < 0)
			continue;

		rv = verify_leader(token, &token->disks[d], &leaders[d], checksum, caller);
		if (rv < 0)
			continue;

		num_reads++;

		leader_reps[d] = 1;

		/* count how many times the same leader block repeats */

		for (i = 0; i < d; i++) {
			if (leaders_match(&leaders[d], &leaders[i])) {
				leader_reps[i]++;
				break;
			}
		}
	}

	if (!majority_disks(num_disks, num_reads)) {
		log_errot(token, "%s leader read error %d", caller, rv);
		error = SANLK_LEADER_READ;
		goto out;
	}

	/* check that a majority of disks have the same leader */

	found = 0;

	for (d = 0; d < num_disks; d++) {
		if (!majority_disks(num_disks, leader_reps[d]))
			continue;

		/* leader on d is the same on a majority of disks,
		   leader becomes the prototype for new_leader */

		memcpy(&leader, &leaders[d], sizeof(struct leader_record));
		found = 1;
		break;
	}

	if (!found) {
		log_errot(token, "%s leader inconsistent", caller);
		error = SANLK_LEADER_DIFF;
		goto out;
	}

	error = SANLK_OK;
 out:
	memcpy(leader_ret, &leader, sizeof(struct leader_record));
	free(leaders);
	free(leader_reps);
	return error;
}

int paxos_lease_leader_read(struct task *task,
			    struct token *token,
			    struct leader_record *leader_ret,
			    const char *caller)
{
	int rv;

	/* _leader_read_num works fine for the single disk case, but
	   we can cut out a bunch of stuff when we know there's one disk */

	if (token->r.num_disks > 1)
		rv = _leader_read_num(task, token, leader_ret, caller);
	else
		rv = _leader_read_one(task, token, leader_ret, caller);

	if (rv == SANLK_OK)
		log_token(token, "%s leader %llu owner %llu %llu %llu", caller,
			  (unsigned long long)leader_ret->lver,
			  (unsigned long long)leader_ret->owner_id,
			  (unsigned long long)leader_ret->owner_generation,
			  (unsigned long long)leader_ret->timestamp);

	return rv;
}

static int _lease_read_one(struct task *task,
			   struct token *token,
			   uint32_t flags,
			   struct sync_disk *disk,
			   struct leader_record *leader_ret,
			   struct paxos_dblock *our_dblock,
			   uint64_t *max_mbal,
			   int *max_q,
			   const char *caller,
			   int log_bk_vals)
{
	char bk_debug[BK_DEBUG_SIZE];
	char bk_str[BK_STR_SIZE];
	int bk_debug_count;
	struct leader_record leader_end;
	struct paxos_dblock our_dblock_end;
	struct paxos_dblock bk;
	char *iobuf, **p_iobuf;
	uint32_t host_id = token->host_id;
	uint32_t sector_size = token->sector_size;
	uint32_t checksum;
	struct paxos_dblock *bk_end;
	uint64_t tmp_mbal = 0;
	int q, tmp_q = -1, rv, iobuf_len;

	iobuf_len = token->align_size;
	if (iobuf_len < 0)
		return iobuf_len;

	p_iobuf = &iobuf;

	rv = posix_memalign((void *)p_iobuf, getpagesize(), iobuf_len);
	if (rv)
		return rv;

	memset(iobuf, 0, iobuf_len);

	rv = read_iobuf(disk->fd, disk->offset, iobuf, iobuf_len, task, token->io_timeout, NULL);
	if (rv < 0)
		goto out;

	memcpy(&leader_end, iobuf, sizeof(struct leader_record));

	checksum = leader_checksum(&leader_end);

	leader_record_in(&leader_end, leader_ret);

	memcpy(&our_dblock_end, iobuf + ((host_id + 1) * sector_size), sizeof(struct paxos_dblock));
	paxos_dblock_in(&our_dblock_end, our_dblock);

	rv = verify_leader(token, disk, leader_ret, checksum, caller);
	if (rv < 0)
		goto out;

	memset(bk_debug, 0, sizeof(bk_debug));
	bk_debug_count = 0;

	for (q = 0; q < leader_ret->num_hosts; q++) {
		bk_end = (struct paxos_dblock *)(iobuf + ((2 + q) * sector_size));

		checksum = dblock_checksum(bk_end);

		paxos_dblock_in(bk_end, &bk);

		if (log_bk_vals && bk.mbal &&
		    ((flags & PAXOS_ACQUIRE_DEBUG_ALL) || (bk.lver >= leader_ret->lver))) {
			if (bk_debug_count >= BK_DEBUG_COUNT) {
				log_token(token, "leader %llu dblocks %s",
					  (unsigned long long)leader_ret->lver, bk_debug);
				memset(bk_debug, 0, sizeof(bk_debug));
				bk_debug_count = 0;
			}

			memset(bk_str, 0, sizeof(bk_str));
			snprintf(bk_str, BK_STR_SIZE, "%d:%llu:%llu:%llu:%llu:%llu:%llu:%x,", q,
				 (unsigned long long)bk.mbal,
				 (unsigned long long)bk.bal,
				 (unsigned long long)bk.inp,
				 (unsigned long long)bk.inp2,
				 (unsigned long long)bk.inp3,
				 (unsigned long long)bk.lver,
				 bk.flags);
			bk_str[BK_STR_SIZE-1] = '\0';
			strncat(bk_debug, bk_str, BK_STR_SIZE-1);
			bk_debug_count++;
		}

		rv = verify_dblock(token, &bk, checksum);
		if (rv < 0)
			goto out;

		if (!tmp_mbal || bk.mbal > tmp_mbal) {
			tmp_mbal = bk.mbal;
			tmp_q = q;
		}
	}
	*max_mbal = tmp_mbal;
	*max_q = tmp_q;

	if (log_bk_vals)
		log_token(token, "leader %llu owner %llu %llu %llu dblocks %s",
			  (unsigned long long)leader_ret->lver,
			  (unsigned long long)leader_ret->owner_id,
			  (unsigned long long)leader_ret->owner_generation,
			  (unsigned long long)leader_ret->timestamp,
			  bk_debug);

 out:
	if (rv != SANLK_AIO_TIMEOUT)
		free(iobuf);
	return rv;
}

/* TODO: completely untested */

static int _lease_read_num(struct task *task,
			   struct token *token,
			   uint32_t flags,
			   struct leader_record *leader_ret,
			   struct paxos_dblock *our_dblock,
			   uint64_t *max_mbal,
			   int *max_q,
			   const char *caller)
{
	struct paxos_dblock dblock_one;
	struct leader_record leader_one;
	struct leader_record *leaders;
	uint64_t tmp_mbal = 0;
	uint64_t mbal_one;
	int *leader_reps;
	int num_disks = token->r.num_disks;
	int leaders_len, leader_reps_len;
	int i, d, rv = 0, found, num_reads, q_one, tmp_q = -1;

	leaders_len = num_disks * sizeof(struct leader_record);
	leader_reps_len = num_disks * sizeof(int);

	leaders = malloc(leaders_len);
	if (!leaders)
		return -ENOMEM;

	leader_reps = malloc(leader_reps_len);
	if (!leader_reps) {
		free(leaders);
		return -ENOMEM;
	}

	memset(leaders, 0, leaders_len);
	memset(leader_reps, 0, leader_reps_len);

	num_reads = 0;

	for (d = 0; d < num_disks; d++) {
		rv = _lease_read_one(task, token, flags, &token->disks[d], &leader_one,
				     &dblock_one, &mbal_one, &q_one, caller, 0);
		if (rv < 0)
			continue;

		num_reads++;

		if (!tmp_mbal || mbal_one > tmp_mbal) {
			tmp_mbal = mbal_one;
			tmp_q = q_one;
			memcpy(our_dblock, &dblock_one, sizeof(struct paxos_dblock));
		}

		memcpy(&leaders[d], &leader_one, sizeof(struct leader_record));

		leader_reps[d] = 1;

		/* count how many times the same leader block repeats */

		for (i = 0; i < d; i++) {
			if (leaders_match(&leaders[d], &leaders[i])) {
				leader_reps[i]++;
				break;
			}
		}
	}
	*max_mbal = tmp_mbal;
	*max_q = tmp_q;

	if (!num_reads) {
		log_errot(token, "%s lease_read_num cannot read disks %d", caller, rv);
		rv = SANLK_DBLOCK_READ;
		goto out;
	}

	found = 0;

	for (d = 0; d < num_disks; d++) {
		if (!majority_disks(num_disks, leader_reps[d]))
			continue;

		/* leader on d is the same on a majority of disks,
		   leader becomes the prototype for new_leader */

		memcpy(leader_ret, &leaders[d], sizeof(struct leader_record));
		found = 1;
		break;
	}

	if (!found) {
		log_errot(token, "%s lease_read_num leader inconsistent", caller);
		rv = SANLK_LEADER_DIFF;
	}
 out:
	free(leaders);
	free(leader_reps);
	return rv;
}

/*
 * read all the initial values needed to start disk paxos:
 * - the leader record
 * - our own dblock
 * - the max mbal from all dblocks
 *
 * Read the entire lease area in one i/o and copy all those
 * values from it.
 */

/**
 * 读取启动磁盘 Paxos 所需的所有初始值：
- 领导者记录
- 我们自己的 dblock
- 所有 dblocks 的最大 mbal
通过一个 I/O 读取整个租约区域，并从中复制所有这些值。
1. leader block
2. our block
3. max bal
 * 
*/

static int paxos_lease_read(struct task *task, struct token *token, uint32_t flags,
			    struct leader_record *leader_ret,
			    uint64_t *max_mbal, const char *caller, int log_bk_vals)
{
	struct paxos_dblock our_dblock;
	int rv, q = -1;

	if (token->r.num_disks > 1)
		rv = _lease_read_num(task, token, flags,
				     leader_ret, &our_dblock, max_mbal, &q, caller);
	else
		rv = _lease_read_one(task, token, flags, &token->disks[0],
				     leader_ret, &our_dblock, max_mbal, &q, caller, log_bk_vals);

	if (rv == SANLK_OK)
		log_token(token, "%s leader %llu owner %llu %llu %llu max mbal[%d] %llu "
			  "our_dblock %llu %llu %llu %llu %llu %llu",
			  caller,
			  (unsigned long long)leader_ret->lver,
			  (unsigned long long)leader_ret->owner_id,
			  (unsigned long long)leader_ret->owner_generation,
			  (unsigned long long)leader_ret->timestamp,
			  q,
			  (unsigned long long)*max_mbal,
			  (unsigned long long)our_dblock.mbal,
			  (unsigned long long)our_dblock.bal,
			  (unsigned long long)our_dblock.inp,
			  (unsigned long long)our_dblock.inp2,
			  (unsigned long long)our_dblock.inp3,
			  (unsigned long long)our_dblock.lver);

	return rv;
}

static int write_new_leader(struct task *task,
			    struct token *token,
			    struct leader_record *nl,
			    const char *caller)
{
	int num_disks = token->r.num_disks;
	int num_writes = 0;
	int timeout = 0;
	int rv = 0;
	int d;

	for (d = 0; d < num_disks; d++) {
		rv = write_leader(task, token, &token->disks[d], nl);
		if (rv == SANLK_AIO_TIMEOUT)
			timeout = 1;
		if (rv < 0) 
			continue;
		num_writes++;
	}

	if (!majority_disks(num_disks, num_writes)) {
		log_errot(token, "%s write_new_leader error %d timeout %d owner %llu %llu %llu",
			  caller, rv, timeout,
			  (unsigned long long)nl->owner_id,
			  (unsigned long long)nl->owner_generation,
			  (unsigned long long)nl->timestamp);
		if (timeout)
			return SANLK_AIO_TIMEOUT;
		if (rv < 0)
			return rv;
		return SANLK_LEADER_WRITE;
	}

	return SANLK_OK;
}

/*
 * If we hang or crash after completing a ballot successfully, but before
 * committing the leader_record, then the next host that runs a ballot (with the
 * same lver since we did not commit the new lver to the leader_record) will
 * commit the same inp values that we were about to commit.  If the inp values
 * they commit indicate we (who crashed or hung) are the new owner, then the
 * other hosts will begin monitoring the liveness of our host_id.  Once enough
 * time has passed, they assume we're dead, and go on with new versions.  The
 * "enough time" ensures that if we hung before writing the leader, that we
 * won't wake up and finally write what will then be an old invalid leader.
 */

/*
 * 如果在成功完成选票后，但在提交leader_record之前挂起或崩溃，那么下一个运行选票的主机
 * （因为我们没有将新的lver提交到leader_record，所以lver相同）将提交与我们即将提交的相同的inp值。
 * 如果他们提交的inp值表明我们（即崩溃或挂起的主机）是新的所有者，那么其他主机将开始监视我们主机的活动状态。
 * 一旦足够的时间过去，它们就会假设我们已经死亡，然后继续进行新版本。足够的时间确保如果我们在编写leader之前挂起，
 * 我们不会醒来并最终编写一个旧的无效leader。
 * lver是什么？
*/

/*
 * i/o required to acquire a free lease
 * (1 disk in token, 512 byte sectors, default num_hosts of 2000)
 *
 * paxos_lease_acquire()
 * 	paxos_lease_read()	1 read   1 MB (entire lease area)
 * 	run_ballot()
 * 		write_dblock()	1 write  512 bytes (1 dblock sector)
 * 		read_iobuf()	1 read   1 MB (round up num_hosts + 2 sectors)
 * 		write_dblock()  1 write  512 bytes (1 dblock sector)
 * 		read_iobuf()	1 read   1 MB (round up num_hosts + 2 sectors)
 * 	write_new_leader()	1 write  512 bytes (1 leader sector)
 *
 * 				6 i/os = 3 1MB reads, 3 512 byte writes
 */

/*
acquire io: read 1 在 retry_ballot 中第一次读
0. 读   1MB ----> 读原来的leader
-------------
1. p1写 512B
2. p1读 1MB
3. p2写 512B
4. p2读 1MB
-------------
5. 写   512B ------> 写新的leader
----------------
共计: 3次读 3*1MB，3次写  3*512B
*/

int paxos_lease_acquire(struct task *task,
			struct token *token,
			uint32_t flags,
		        struct leader_record *leader_ret,
			struct paxos_dblock *dblock_ret,
		        uint64_t acquire_lver,
		        int new_num_hosts)
{
	struct sync_disk host_id_disk;
	/*这几个leader有什么用？*/
	struct leader_record host_id_leader;
	struct leader_record cur_leader;
	struct leader_record tmp_leader;
	struct leader_record new_leader;

	struct paxos_dblock dblock;
	struct paxos_dblock owner_dblock;
	struct host_status hs;
	uint64_t wait_start, now;
	uint64_t last_timestamp;
	uint64_t next_lver;
	uint64_t max_mbal;
	uint64_t num_mbal;
	uint64_t our_mbal;
	int copy_cur_leader;
	int disk_open = 0;
	int error, rv, us;
	int align_size;
	int ls_sector_size;
	int other_io_timeout, other_host_dead_seconds;

	memset(&dblock, 0, sizeof(dblock)); /* shut up compiler */
	/*token是什么？*/
	log_token(token, "paxos_acquire begin offset %llu 0x%x %d %d",
		  (unsigned long long)token->disks[0].offset, flags,
		  token->sector_size, token->align_size);

	if (!token->sector_size) {
		log_errot(token, "paxos_acquire with zero sector_size");
		return -EINVAL;
	}

 restart:
	memset(&tmp_leader, 0, sizeof(tmp_leader));
	copy_cur_leader = 0;

	/* acquire io: read 1 */
	/*1.这里会读1MB，目的是干啥呢？从这里读出cur_leader*/
	/* 这个token很奇怪呀？读磁盘后记录的什么信息呢？ 
	注意：这里是lease_read 不知道这种读有什么奇怪的地方
	force acquire 有没有可能会在这里读失败？？？
	*/
	error = paxos_lease_read(task, token, flags, &cur_leader, &max_mbal, "paxos_acquire", 1);
	if (error < 0)
		goto out;

	align_size = leader_align_size_from_flag(cur_leader.flags);
	if (!align_size)
		align_size = sector_size_to_align_size_old(cur_leader.sector_size);

	/*
	 * token sector_size/align_size are initially set from the lockspace values,
	 * and paxos_lease_read() uses these values.  It's possible but unusual
	 * that the paxos lease leader record will have different sector/align
	 * sizes than we used initially.
	 */
	if ((cur_leader.sector_size != token->sector_size) ||
	    (align_size != token->align_size)) {
		log_token(token, "paxos_acquire restart with different sizes was %d %d now %d %d",
			  token->sector_size, token->align_size,
			  cur_leader.sector_size, align_size);
		token->sector_size = cur_leader.sector_size;
		token->align_size = align_size;
		goto restart;
	}

	if (flags & PAXOS_ACQUIRE_FORCE) {
		copy_cur_leader = 1;
		goto run;
	}
	// direct模式下这个acquire_lver=0
	if (acquire_lver && cur_leader.lver != acquire_lver) {
		log_errot(token, "paxos_acquire acquire_lver %llu cur_leader %llu",
			  (unsigned long long)acquire_lver,
			  (unsigned long long)cur_leader.lver);
		error = SANLK_ACQUIRE_LVER;
		goto out;
	}
	// 如果时间戳是0，则直接run
	if (cur_leader.timestamp == LEASE_FREE) {
		log_token(token, "paxos_acquire leader %llu free",
			  (unsigned long long)cur_leader.lver);
		copy_cur_leader = 1;
		goto run;
	}
	// owner_id和token的id必须一致？？？token
	if (cur_leader.owner_id == token->host_id &&
	    cur_leader.owner_generation == token->host_generation) {
		log_token(token, "paxos_acquire owner %llu %llu %llu is already local %llu %llu",
			  (unsigned long long)cur_leader.owner_id,
			  (unsigned long long)cur_leader.owner_generation,
			  (unsigned long long)cur_leader.timestamp,
			  (unsigned long long)token->host_id,
			  (unsigned long long)token->host_generation);
		copy_cur_leader = 1;
		goto run;
	}

	/*
	 * We were the last host to hold this lease, but in a previous
	 * lockspace generation in which we didn't cleanly release the
	 * paxos lease.
	 */

	if (cur_leader.owner_id == token->host_id &&
	    cur_leader.owner_generation < token->host_generation) {
		log_token(token, "paxos_acquire owner %llu %llu %llu was old local new is %llu",
			  (unsigned long long)cur_leader.owner_id,
			  (unsigned long long)cur_leader.owner_generation,
			  (unsigned long long)cur_leader.timestamp,
			  (unsigned long long)token->host_generation);
		copy_cur_leader = 1;
		goto run;
	}

	/*
	 * Check if current owner is alive based on its host_id renewals.
	 * If the current owner has been dead long enough we can assume that
	 * its watchdog has triggered and we can go for the paxos lease.
	 */

	if (!disk_open) {
		memset(&host_id_disk, 0, sizeof(host_id_disk));

		rv = lockspace_disk(cur_leader.space_name, &host_id_disk, &ls_sector_size);
		if (rv < 0) {
			log_errot(token, "paxos_acquire no lockspace info %.48s",
			  	  cur_leader.space_name);
			error = SANLK_ACQUIRE_LOCKSPACE;
			goto out;
		}
		host_id_disk.fd = -1;

		rv = open_disks_fd(&host_id_disk, 1);
		if (rv < 0) {
			log_errot(token, "paxos_acquire open host_id_disk error %d", rv);
			error = SANLK_ACQUIRE_IDDISK;
			goto out;
		}
		disk_open = 1;
	}

	rv = host_info(cur_leader.space_name, cur_leader.owner_id, &hs);
	if (!rv && hs.last_check && hs.last_live &&
	    hs.owner_id == cur_leader.owner_id &&
	    hs.owner_generation == cur_leader.owner_generation) {
		wait_start = hs.last_live;
		last_timestamp = hs.timestamp;
	} else {
		wait_start = monotime();
		last_timestamp = 0;
	}

	log_token(token, "paxos_acquire owner %llu %llu %llu "
		  "host_status %llu %llu %llu wait_start %llu",
		  (unsigned long long)cur_leader.owner_id,
		  (unsigned long long)cur_leader.owner_generation,
		  (unsigned long long)cur_leader.timestamp,
		  (unsigned long long)hs.owner_id,
		  (unsigned long long)hs.owner_generation,
		  (unsigned long long)hs.timestamp,
		  (unsigned long long)wait_start);

	while (1) {
		// 如果是direct模式也会走delta_lease判断么？这是获取什么？leader信息吗？
		error = delta_lease_leader_read(task, ls_sector_size, token->io_timeout,
						&host_id_disk,
						cur_leader.space_name,
						cur_leader.owner_id,
						&host_id_leader,
						"paxos_acquire");
		if (error < 0) {
			log_errot(token, "paxos_acquire owner %llu %llu %llu "
				  "delta read %d fd %d path %s off %llu",
				  (unsigned long long)cur_leader.owner_id,
				  (unsigned long long)cur_leader.owner_generation,
				  (unsigned long long)cur_leader.timestamp,
				  error, host_id_disk.fd, host_id_disk.path,
				  (unsigned long long)host_id_disk.offset);
			goto out;
		}

		/* a host_id cannot become free in less than
		   host_dead_seconds after the final renewal because
		   a host_id must first be acquired before being freed,
		   and acquiring cannot take less than host_dead_seconds */
		// 如果时间戳是0则直接获取
		if (host_id_leader.timestamp == LEASE_FREE) {
			log_token(token, "paxos_acquire owner %llu delta free",
				  (unsigned long long)cur_leader.owner_id);
			goto run;
		}

		/* another host has acquired the host_id of the host that
		   owned this paxos lease; acquiring a host_id also cannot be
		   done in less than host_dead_seconds, or

		   the host_id that owns this lease may be alive, but it
		   owned the lease in a previous generation without freeing it,
		   and no longer owns it */
		/*如果从磁盘读到的领导者host_id_leader，跟当前传进来的ID不同，或者磁盘上的Leader的gen更大，
		那么就进入run*/ 
		if (host_id_leader.owner_id != cur_leader.owner_id ||
		    host_id_leader.owner_generation > cur_leader.owner_generation) {
			log_token(token, "paxos_acquire owner %llu %llu %llu "
				  "delta %llu %llu %llu mismatch",
				  (unsigned long long)cur_leader.owner_id,
				  (unsigned long long)cur_leader.owner_generation,
				  (unsigned long long)cur_leader.timestamp,
				  (unsigned long long)host_id_leader.owner_id,
				  (unsigned long long)host_id_leader.owner_generation,
				  (unsigned long long)host_id_leader.timestamp);
			goto run;
		}
		// 如果时间戳为空，那么什么时候设置时间戳为磁盘上读到的时间戳
		if (!last_timestamp) {
			last_timestamp = host_id_leader.timestamp;
			goto skip_live_check;
		}

		/*
		 * Check if the owner is alive:
		 *
		 * 1. We just read the delta lease of the owner (host_id_leader).
		 * If that has a newer timestamp than the timestamp last seen by
		 * our own renewal thread (last_timestamp), then the owner is alive.
		 *
		 * 2. If our own renewal thread saw the owner's timestamp change
		 * the last time it was checked, then consider the owner to be alive.
		 */
		/*
		* 检查所有者是否存活：
		*
		* 1. 我们刚刚读取了所有者（host_id_leader）的增量租约。
		* 如果该增量租约的时间戳比我们自己续订线程上次看到的时间戳（last_timestamp）要新，则表示所有者存活。
		*
		* 2. 如果我们自己的续订线程在上次检查时看到了所有者的时间戳变化，则认为所有者存活。
		*/

		if ((host_id_leader.timestamp != last_timestamp) ||
		    (hs.last_live && (hs.last_check == hs.last_live))) {
			log_token(token, "paxos_acquire owner %llu delta %llu %llu %llu alive",
				  (unsigned long long)cur_leader.owner_id,
				  (unsigned long long)host_id_leader.owner_id,
				  (unsigned long long)host_id_leader.owner_generation,
				  (unsigned long long)host_id_leader.timestamp);
			memcpy(leader_ret, &cur_leader, sizeof(struct leader_record));

			/* It's possible that the live owner has released the
			   lease, but its release was clobbered by another host
			   that was running the ballot with it and wrote it as
			   the owner.  If the leader writer was not the owner,
			   check if the owner's dblock is cleared.  If so, then
			   the owner released the lease and we can run a
			   ballot.  Comparing the write_id and owner_id is not
			   required; we could always read the owner dblock
			   here, but comparing the writer and owner can
			   eliminate many unnecessary dblock reads. */

			if (cur_leader.write_id != cur_leader.owner_id) {
				rv = read_dblock(task, token, &token->disks[0],
						 cur_leader.owner_id, &owner_dblock);
				if (!rv && (owner_dblock.flags & DBLOCK_FL_RELEASED)) {
					/* not an error, but interesting to see */
					log_warnt(token, "paxos_acquire owner %llu %llu %llu writer %llu owner dblock released",
						  (unsigned long long)cur_leader.owner_id,
						  (unsigned long long)cur_leader.owner_generation,
						  (unsigned long long)cur_leader.timestamp,
						  (unsigned long long)cur_leader.write_id);
					goto run;
				}
			}

			error = SANLK_ACQUIRE_IDLIVE;
			goto out;
		}

		/* If the owner hasn't renewed its host_id lease for
		   host_dead_seconds then its watchdog should have fired by
		   now. */
		/* 如果所有者在 host_dead_seconds 时间内没有更新其主机 ID 租约，
		则其看门狗应该已经触发了。 */
		now = monotime();

		other_io_timeout = hs.io_timeout;
		other_host_dead_seconds = calc_host_dead_seconds(other_io_timeout);

		if (now - wait_start > other_host_dead_seconds) {
			log_token(token, "paxos_acquire owner %llu %llu %llu "
				  "delta %llu %llu %llu dead %llu-%llu>%d",
				  (unsigned long long)cur_leader.owner_id,
				  (unsigned long long)cur_leader.owner_generation,
				  (unsigned long long)cur_leader.timestamp,
				  (unsigned long long)host_id_leader.owner_id,
				  (unsigned long long)host_id_leader.owner_generation,
				  (unsigned long long)host_id_leader.timestamp,
				  (unsigned long long)now,
				  (unsigned long long)wait_start,
				  other_host_dead_seconds);
			goto run;
		}

		if (flags & PAXOS_ACQUIRE_OWNER_NOWAIT) {
			log_token(token, "paxos_acquire owner %llu %llu %llu no wait",
				  (unsigned long long)cur_leader.owner_id,
				  (unsigned long long)cur_leader.owner_generation,
				  (unsigned long long)cur_leader.timestamp);
			error = SANLK_ACQUIRE_OWNED_RETRY;
			goto out;
		}

 skip_live_check:
		/* TODO: test with sleep(2) here */
		sleep(1);

		if (external_shutdown) {
			error = -1;
			goto out;
		}

		/*
		 * In this while loop we are waiting for an indication that the
		 * current owner is alive or dead, but if we see the leader
		 * owner change in the meantime, we'll restart the entire
		 * process.
		 */
		/*
		* 在这个 while 循环中，我们正在等待当前所有者是存活还是死亡的指示，
		* 但是如果在此期间看到领导者所有者发生变化，我们将重新启动整个过程。
		*/
		/*
		* cur_leader: 是第一次读到的
		* tmp_leader: 是刚刚读到的
		*/
		error = paxos_lease_leader_read(task, token, &tmp_leader, "paxos_acquire");
		if (error < 0)
			goto out;

		if (memcmp(&cur_leader, &tmp_leader, sizeof(struct leader_record))) {
			log_token(token, "paxos_acquire restart leader changed1 from "
				  "%llu %llu %llu to %llu %llu %llu",
				  (unsigned long long)cur_leader.owner_id,
				  (unsigned long long)cur_leader.owner_generation,
				  (unsigned long long)cur_leader.timestamp,
				  (unsigned long long)tmp_leader.owner_id,
				  (unsigned long long)tmp_leader.owner_generation,
				  (unsigned long long)tmp_leader.timestamp);
			goto restart;
		}
	}
 run:
	/*
	 * Use the disk paxos algorithm to attempt to commit a new leader.
	 *
	 * If we complete a ballot successfully, we can commit a leader record
	 * with next_lver.  If we find a higher mbal during a ballot, we increase
	 * our own mbal and try the ballot again.
	 *
	 * next_lver is derived from cur_leader with a zero or timed out owner.
	 * We need to monitor the leader record to see if another host commits
	 * a new leader_record with next_lver.
	 *
	 * TODO: may not need to increase mbal if dblock.inp and inp2 match
	 * current host_id and generation?
	 */

	/* This next_lver assignment is based on the original cur_leader, not a
	   re-reading of the leader here, i.e. we cannot just re-read the leader
	   here, and make next_lver one more than that.  This is because another
	   node may have made us the owner of next_lver as it is now. */
	/*使用磁盘 Paxos 算法尝试提交新的领导者。
	如果我们成功完成一轮选举，我们可以使用 next_lver 提交一个领导者记录。如果在选举期间发现了更高的 mbal，我们会增加自己的 mbal 并重新尝试选举。
	next_lver 是从当前领导者（cur_leader）派生出来的，其所有者是零或超时。我们需要监视领导者记录，以查看另一个主机是否提交了一个带有 next_lver 的新 leader_record。
	
	待办事项：如果 dblock.inp 和 inp2 与当前的 host_id 和 generation 匹配，则可能不需要增加 mbal？
	此次 next_lver 分配基于原始的 cur_leader，而不是此处重新读取领导者，即我们不能在此处重新读取领导者，然后将 next_lver 设置为比原始领导者更大一个。
	这是因为另一个节点现在可能已经将我们指定为 next_lver 的所有者。
	*/
	
	// 这里的lver是我们第一次读到的lver+1
	next_lver = cur_leader.lver + 1;
	// 如果最大提案号为0
	if (!max_mbal) {
		our_mbal = token->host_id;
	} else {
		// 如果提案号不为0，则增加全部的host数，然后加上自己的编号
		num_mbal = max_mbal - (max_mbal % cur_leader.max_hosts);
		our_mbal = num_mbal + cur_leader.max_hosts + token->host_id;
	}

 retry_ballot:
	/* 在这里可以节省一次IO，为什么能省？ */
	if (copy_cur_leader) {
		/* reusing the initial read removes an iop in the common case */
		copy_cur_leader = 0;
		memcpy(&tmp_leader, &cur_leader, sizeof(struct leader_record));
	} else {
		/* acquire io: read 1 (for retry) */
		error = paxos_lease_leader_read(task, token, &tmp_leader, "paxos_acquire");
		if (error < 0)
			goto out;
	}
	/* 如果其他节点已经提交了leader信息 */
	if (tmp_leader.lver == next_lver) {
		/*
		 * another host has commited a leader_record for next_lver,
		 * check which inp (owner_id) they commited (possibly us).
		 */

		if (tmp_leader.owner_id == token->host_id &&
		    tmp_leader.owner_generation == token->host_generation) {
			/* not a problem, but interesting to see */

			log_warnt(token, "paxos_acquire %llu owner is our inp "
				  "%llu %llu %llu commited by %llu",
				  (unsigned long long)next_lver,
				  (unsigned long long)tmp_leader.owner_id,
				  (unsigned long long)tmp_leader.owner_generation,
				  (unsigned long long)tmp_leader.timestamp,
				  (unsigned long long)tmp_leader.write_id);

			memcpy(leader_ret, &tmp_leader, sizeof(struct leader_record));
			memcpy(dblock_ret, &dblock, sizeof(struct paxos_dblock));
			error = SANLK_OK;
		} else {
			/* not a problem, but interesting to see */

			log_warnt(token, "paxos_acquire %llu owner is %llu %llu %llu",
				  (unsigned long long)next_lver,
				  (unsigned long long)tmp_leader.owner_id,
				  (unsigned long long)tmp_leader.owner_generation,
				  (unsigned long long)tmp_leader.timestamp);

			memcpy(leader_ret, &tmp_leader, sizeof(struct leader_record));
			error = SANLK_ACQUIRE_OWNED;
		}
		goto out;
	}
	/* 如果读到的leader version比我们即将要写入的version还要大，那当前是不是就只能重试了 */
	if (tmp_leader.lver > next_lver) {
		/*
		 * A case where this was observed: for next_lver 65 we abort1, and delay.
		 * While sleeping, the lease v65 (which was acquired during our abort1) is
		 * released and then reacquired as v66.  When we goto retry_ballot, our
		 * next_lver is 65, but the current lver on disk is 66, causing us to
		 * we fail in the larger1 check.)
		 */
		/*
		这里观察到的一种情况：对于 next_lver 65，我们执行了 abort1 并延迟。在睡眠期间，租约 v65（在我们的 abort1 过程中获得）被释放，然后重新作为 v66 获得。当我们转到 retry_ballot 时，我们的 next_lver 是 65，但磁盘上当前的 lver 是 66，导致我们在 larger1 检查中失败。
		*/
		log_token(token, "paxos_acquire %llu restart new lver %llu from "
			  "%llu %llu %llu to %llu %llu %llu",
			  (unsigned long long)next_lver,
			  (unsigned long long)tmp_leader.lver,
			  (unsigned long long)cur_leader.owner_id,
			  (unsigned long long)cur_leader.owner_generation,
			  (unsigned long long)cur_leader.timestamp,
			  (unsigned long long)tmp_leader.owner_id,
			  (unsigned long long)tmp_leader.owner_generation,
			  (unsigned long long)tmp_leader.timestamp);
		goto restart;
	}
	// 比较两个Leader是否相等，相等返回0
	if (memcmp(&cur_leader, &tmp_leader, sizeof(struct leader_record))) {
		log_token(token, "paxos_acquire %llu restart leader changed2 from "
			  "%llu %llu %llu to %llu %llu %llu",
			  (unsigned long long)next_lver,
			  (unsigned long long)cur_leader.owner_id,
			  (unsigned long long)cur_leader.owner_generation,
			  (unsigned long long)cur_leader.timestamp,
			  (unsigned long long)tmp_leader.owner_id,
			  (unsigned long long)tmp_leader.owner_generation,
			  (unsigned long long)tmp_leader.timestamp);
		goto restart;
	}
	// 这里进行选举
	error = run_ballot(task, token, flags, cur_leader.num_hosts, next_lver, our_mbal, &dblock);
	// 什么情况下需要重新进行选举？
	if ((error == SANLK_DBLOCK_MBAL) || (error == SANLK_DBLOCK_LVER)) {
		// 从1秒内随机睡眠
		us = get_rand(0, 1000000);
		if (us < 0)
			us = token->host_id * 100;

		log_token(token, "paxos_acquire %llu retry delay %d us",
			  (unsigned long long)next_lver, us);

		usleep(us);
		our_mbal += cur_leader.max_hosts;
		goto retry_ballot;
	}

	if (error < 0) {
		log_errot(token, "paxos_acquire %llu ballot error %d",
			  (unsigned long long)next_lver, error);
		goto out;
	}

	/* ballot success, commit next_lver with dblock values */
	/* 选举成功，将dblock的值写入磁盘 */
	memcpy(&new_leader, &cur_leader, sizeof(struct leader_record));
	new_leader.lver = dblock.lver;
	// 这三个值就是我们想写入的值
	new_leader.owner_id = dblock.inp;
	new_leader.owner_generation = dblock.inp2;
	new_leader.timestamp = dblock.inp3;
	// 写入之前的这三个值也记录了
	new_leader.write_id = token->host_id;
	new_leader.write_generation = token->host_generation;
	new_leader.write_timestamp = monotime();

	if (new_num_hosts)
		new_leader.num_hosts = new_num_hosts;
	// 这个判断语句说明什么？似乎是为了加速其他节点失败的判断
	if (new_leader.owner_id == token->host_id) {
		/*
		 * The LFL_SHORT_HOLD flag is just a "hint" to help
		 * other nodes be more intelligent about retrying
		 * due to transient failures when acquiring shared
		 * leases.  Only modify SHORT_HOLD if we're committing
		 * ourself as the new owner.  If we're committing another
		 * host as owner, we don't know if they are acquiring
		 * shared or not.
		 */
		if (flags & PAXOS_ACQUIRE_SHARED)
			new_leader.flags |= LFL_SHORT_HOLD;
		else
			new_leader.flags &= ~LFL_SHORT_HOLD;
	}

	new_leader.checksum = 0; /* set after leader_record_out */
	// 把新的leader写入磁盘，写入之后会修改checksum的值
	error = write_new_leader(task, token, &new_leader, "paxos_acquire");
	if (error < 0) {
		/* See comment in run_ballot about this flag. */
		/* 这里是为了不让自己升为Leader做的标识 */
		token->flags |= T_RETRACT_PAXOS;
		memcpy(leader_ret, &new_leader, sizeof(struct leader_record));
		goto out;
	}
	// 判断leader.owner_id 跟xx是否相等
	if (new_leader.owner_id != token->host_id) {
		/* not a problem, but interesting to see */

		/* It's possible that we commit an outdated owner id/gen here.
		   If we go back to the top and retry, we may find that the
		   owner host_id is alive but with a newer generation, and
		   we'd be able to get the lease by running the ballot again. */

		log_warnt(token, "ballot %llu commit other owner %llu %llu %llu",
			  (unsigned long long)new_leader.lver,
			  (unsigned long long)new_leader.owner_id,
			  (unsigned long long)new_leader.owner_generation,
			  (unsigned long long)new_leader.timestamp);

		memcpy(leader_ret, &new_leader, sizeof(struct leader_record));
		error = SANLK_ACQUIRE_OTHER;
		goto out;
	}

	log_token(token, "ballot %llu commit self owner %llu %llu %llu",
		  (unsigned long long)next_lver,
		  (unsigned long long)new_leader.owner_id,
		  (unsigned long long)new_leader.owner_generation,
		  (unsigned long long)new_leader.timestamp);

	memcpy(leader_ret, &new_leader, sizeof(struct leader_record));
	memcpy(dblock_ret, &dblock, sizeof(struct paxos_dblock));
	error = SANLK_OK;

 out:
	if (disk_open)
		close_disks(&host_id_disk, 1);

	return error;
}

#if 0
int paxos_lease_renew(struct task *task,
		      struct token *token,
		      struct leader_record *leader_last,
		      struct leader_record *leader_ret)
{
	struct leader_record new_leader;
	int rv, d;
	int error;

	for (d = 0; d < token->r.num_disks; d++) {
		memset(&new_leader, 0, sizeof(struct leader_record));

		rv = read_leader(task, token, &token->disks[d], &new_leader);
		if (rv < 0)
			continue;

		if (memcmp(&new_leader, leader_last,
			   sizeof(struct leader_record))) {
			log_errot(token, "leader changed between renewals");
			return SANLK_BAD_LEADER;
		}
	}

	new_leader.timestamp = monotime();
	new_leader.checksum = 0; /* set after leader_record_out */

	error = write_new_leader(task, token, &new_leader);
	if (error < 0)
		goto out;

	memcpy(leader_ret, &new_leader, sizeof(struct leader_record));
 out:
	return error;
}
#endif

int paxos_lease_release(struct task *task,
			struct token *token,
			struct sanlk_resource *resrename,
		        struct leader_record *leader_last,
		        struct leader_record *leader_ret)
{
	struct leader_record leader;
	struct leader_record *last;
	int error;

	error = paxos_lease_leader_read(task, token, &leader, "paxos_release");
	if (error < 0) {
		log_errot(token, "paxos_release leader_read error %d", error);
		goto out;
	}

	/*
	 * Used when the caller does not know who the owner is, but
	 * wants to ensure it is not the owner.
	 */
	if (!leader_last)
		last = &leader;
	else
		last = leader_last;

	/*
	 * This will happen when two hosts finish the same ballot
	 * successfully, the second committing the same inp values
	 * that the first did, as it should.  But the second will
	 * write it's own write_id/gen/timestamp, which will differ
	 * from what the first host wrote.  So when the first host
	 * rereads here in the release, it will find different
	 * write_id/gen/timestamp from what it wrote.  This is
	 * perfectly fine (use log warn since it's interesting
	 * to see when this happens.)
	 *
	 * If another host was the writer and committed us as the
	 * owner, then we don't zero the leader record when we release,
	 * we just release our dblock (by setting the release flag,
	 * already done prior to calling paxos_lease_release).  This is
	 * because other hosts will ignore our leader record if we were
	 * not the writer once we release our dblock.  Those other
	 * hosts will then run a ballot and commit/write a new leader.
	 * If we are also zeroing the leader, that can race with
	 * another host writing a new leader, and we could clobber the
	 * new leader.
	 */
	/*这里为什么要跟token判断呀？token都没赋值过肯定没数据呀？*/
	if (leader.write_id != token->host_id) {
		log_warnt(token, "paxos_release skip write "
			  "last lver %llu owner %llu %llu %llu writer %llu %llu %llu "
			  "disk lver %llu owner %llu %llu %llu writer %llu %llu %llu",
			  (unsigned long long)last->lver,
			  (unsigned long long)last->owner_id,
			  (unsigned long long)last->owner_generation,
			  (unsigned long long)last->timestamp,
			  (unsigned long long)last->write_id,
			  (unsigned long long)last->write_generation,
			  (unsigned long long)last->write_timestamp,
			  (unsigned long long)leader.lver,
			  (unsigned long long)leader.owner_id,
			  (unsigned long long)leader.owner_generation,
			  (unsigned long long)leader.timestamp,
			  (unsigned long long)leader.write_id,
			  (unsigned long long)leader.write_generation,
			  (unsigned long long)leader.write_timestamp);
		return 0;
	}

	/*
	 * When we were the writer of our own leader record, then
	 * releasing the lease includes both setting the RELEASED flag
	 * in our dblock and clearing out timestamp in the leader.
	 * When we reread the leader here in release, we should find
	 * it the same as we last saw in acquire.
	 */

	if (leader.lver != last->lver) {
		log_errot(token, "paxos_release other lver "
			  "last lver %llu owner %llu %llu %llu writer %llu %llu %llu "
			  "disk lver %llu owner %llu %llu %llu writer %llu %llu %llu",
			  (unsigned long long)last->lver,
			  (unsigned long long)last->owner_id,
			  (unsigned long long)last->owner_generation,
			  (unsigned long long)last->timestamp,
			  (unsigned long long)last->write_id,
			  (unsigned long long)last->write_generation,
			  (unsigned long long)last->write_timestamp,
			  (unsigned long long)leader.lver,
			  (unsigned long long)leader.owner_id,
			  (unsigned long long)leader.owner_generation,
			  (unsigned long long)leader.timestamp,
			  (unsigned long long)leader.write_id,
			  (unsigned long long)leader.write_generation,
			  (unsigned long long)leader.write_timestamp);
		return SANLK_RELEASE_LVER;
	}

	if (leader.timestamp == LEASE_FREE) {
		log_errot(token, "paxos_release already free "
			  "last lver %llu owner %llu %llu %llu writer %llu %llu %llu "
			  "disk lver %llu owner %llu %llu %llu writer %llu %llu %llu",
			  (unsigned long long)last->lver,
			  (unsigned long long)last->owner_id,
			  (unsigned long long)last->owner_generation,
			  (unsigned long long)last->timestamp,
			  (unsigned long long)last->write_id,
			  (unsigned long long)last->write_generation,
			  (unsigned long long)last->write_timestamp,
			  (unsigned long long)leader.lver,
			  (unsigned long long)leader.owner_id,
			  (unsigned long long)leader.owner_generation,
			  (unsigned long long)leader.timestamp,
			  (unsigned long long)leader.write_id,
			  (unsigned long long)leader.write_generation,
			  (unsigned long long)leader.write_timestamp);
		return SANLK_RELEASE_OWNER;
	}

	if (leader.owner_id != token->host_id ||
	    leader.owner_generation != token->host_generation) {
		log_errot(token, "paxos_release other owner "
			  "last lver %llu owner %llu %llu %llu writer %llu %llu %llu "
			  "disk lver %llu owner %llu %llu %llu writer %llu %llu %llu",
			  (unsigned long long)last->lver,
			  (unsigned long long)last->owner_id,
			  (unsigned long long)last->owner_generation,
			  (unsigned long long)last->timestamp,
			  (unsigned long long)last->write_id,
			  (unsigned long long)last->write_generation,
			  (unsigned long long)last->write_timestamp,
			  (unsigned long long)leader.lver,
			  (unsigned long long)leader.owner_id,
			  (unsigned long long)leader.owner_generation,
			  (unsigned long long)leader.timestamp,
			  (unsigned long long)leader.write_id,
			  (unsigned long long)leader.write_generation,
			  (unsigned long long)leader.write_timestamp);
		return SANLK_RELEASE_OWNER;
	}

	if (memcmp(&leader, last, sizeof(struct leader_record))) {
		log_errot(token, "paxos_release different vals "
			  "last lver %llu owner %llu %llu %llu writer %llu %llu %llu "
			  "disk lver %llu owner %llu %llu %llu writer %llu %llu %llu",
			  (unsigned long long)last->lver,
			  (unsigned long long)last->owner_id,
			  (unsigned long long)last->owner_generation,
			  (unsigned long long)last->timestamp,
			  (unsigned long long)last->write_id,
			  (unsigned long long)last->write_generation,
			  (unsigned long long)last->write_timestamp,
			  (unsigned long long)leader.lver,
			  (unsigned long long)leader.owner_id,
			  (unsigned long long)leader.owner_generation,
			  (unsigned long long)leader.timestamp,
			  (unsigned long long)leader.write_id,
			  (unsigned long long)leader.write_generation,
			  (unsigned long long)leader.write_timestamp);
		return SANLK_RELEASE_OWNER;
	}

	if (resrename)
		memcpy(leader.resource_name, resrename->name, NAME_ID_SIZE);

	leader.timestamp = LEASE_FREE;
	leader.write_id = token->host_id;
	leader.write_generation = token->host_generation;
	leader.write_timestamp = monotime();
	leader.flags &= ~LFL_SHORT_HOLD;
	leader.checksum = 0; /* set after leader_record_out */

	error = write_new_leader(task, token, &leader, "paxos_release");
	if (error < 0)
		goto out;

	memcpy(leader_ret, &leader, sizeof(struct leader_record));
 out:
	return error;
}

int paxos_lease_init(struct task *task,
		     struct token *token,
		     int num_hosts, int write_clear)
{
	char *iobuf, **p_iobuf;
	struct leader_record leader;
	struct leader_record leader_end;
	struct request_record rr;
	struct request_record rr_end;
	uint32_t checksum;
	int iobuf_len;
	int sector_size = 0;
	int align_size = 0;
	int max_hosts = 0;
	int aio_timeout = 0;
	int rv, d;

	rv = sizes_from_flags(token->r.flags, &sector_size, &align_size, &max_hosts, "RES");
	if (rv)
		return rv;

	if (!sector_size) {
		/* sector/align flags were not set, use historical defaults */
		sector_size = token->disks[0].sector_size;
		align_size = sector_size_to_align_size_old(sector_size);
		max_hosts = DEFAULT_MAX_HOSTS;
	}

	if (!num_hosts || (num_hosts > max_hosts))
		num_hosts = max_hosts;

	token->sector_size = sector_size;
	token->align_size = align_size;

	iobuf_len = align_size;

	p_iobuf = &iobuf;

	rv = posix_memalign((void *)p_iobuf, getpagesize(), iobuf_len);
	if (rv)
		return rv;

	memset(iobuf, 0, iobuf_len);

	memset(&leader, 0, sizeof(leader));

	if (write_clear) {
		leader.magic = PAXOS_DISK_CLEAR;
		leader.write_timestamp = monotime();
	} else {
		leader.magic = PAXOS_DISK_MAGIC;
	}

	leader.timestamp = LEASE_FREE;
	leader.version = PAXOS_DISK_VERSION_MAJOR | PAXOS_DISK_VERSION_MINOR;
	leader.flags = leader_align_flag_from_size(align_size);
	leader.sector_size = sector_size;
	leader.num_hosts = num_hosts;
	leader.max_hosts = max_hosts;
	strncpy(leader.space_name, token->r.lockspace_name, NAME_ID_SIZE);
	strncpy(leader.resource_name, token->r.name, NAME_ID_SIZE);
	leader.checksum = 0; /* set after leader_record_out */

	memset(&rr, 0, sizeof(rr));
	rr.magic = REQ_DISK_MAGIC;
	rr.version = REQ_DISK_VERSION_MAJOR | REQ_DISK_VERSION_MINOR;

	leader_record_out(&leader, &leader_end);

	/*
	 * N.B. must compute checksum after the data has been byte swapped.
	 */
	checksum = leader_checksum(&leader_end);
	leader.checksum = checksum;
	leader_end.checksum = cpu_to_le32(checksum);

	request_record_out(&rr, &rr_end);

	memcpy(iobuf, &leader_end, sizeof(struct leader_record));
	memcpy(iobuf + sector_size, &rr_end, sizeof(struct request_record));

	for (d = 0; d < token->r.num_disks; d++) {
		rv = write_iobuf(token->disks[d].fd, token->disks[d].offset,
				 iobuf, iobuf_len, task, token->io_timeout, NULL);

		if (rv == SANLK_AIO_TIMEOUT)
			aio_timeout = 1;

		if (rv < 0)
			return rv;
	}

	if (!aio_timeout)
		free(iobuf);

	return 0;
}

