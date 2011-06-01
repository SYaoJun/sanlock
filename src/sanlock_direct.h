/*
 * Copyright (C) 2010-2011 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 */

#ifndef __SANLOCK_DIRECT_H__
#define __SANLOCK_DIRECT_H__

/*
 * Use io_timeout_sec = 0 for default value
 */

int sanlock_direct_read_id(struct sanlk_lockspace *ls,
                           uint64_t *timestamp,
                           uint64_t *owner_id,
                           uint64_t *owner_generation,
                           int use_aio,
			   int io_timeout_sec);

int sanlock_direct_live_id(struct sanlk_lockspace *ls,
                           uint64_t *timestamp,
                           uint64_t *owner_id,
                           uint64_t *owner_generation,
                           int *live,
                           int use_aio,
			   int io_timeout_sec);

/*
 * Use max_hosts = 0 for default max_hosts value
 *
 * Provide either lockspace or resource, not both
 */

int sanlock_direct_init(struct sanlk_lockspace *ls,
                        struct sanlk_resource *res,
                        int max_hosts, int num_hosts, int use_aio);

/*
 * Returns sector size in bytes, -1 on error
 */

int sanlock_direct_sector_size(struct sanlk_disk *disk);

#endif
