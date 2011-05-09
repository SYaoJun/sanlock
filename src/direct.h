/*
 * Copyright (C) 2010-2011 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 */

#ifndef __DIRECT_H__
#define __DIRECT_H__

int direct_acquire(struct timeout *ti,
                   struct sanlk_resource *res,
                   int num_hosts,
                   uint64_t local_host_id,
                   uint64_t local_host_generation,
		   struct leader_record *leader_ret);

int direct_release(struct timeout *ti,
                   struct sanlk_resource *res,
		   struct leader_record *leader_ret);

int direct_acquire_id(struct timeout *ti, struct sanlk_lockspace *ls);
int direct_release_id(struct timeout *ti, struct sanlk_lockspace *ls);
int direct_renew_id(struct timeout *ti, struct sanlk_lockspace *ls);

int direct_read_id(struct timeout *ti,
                   struct sanlk_lockspace *ls,
                   uint64_t *timestamp,
                   uint64_t *owner_id,
                   uint64_t *owner_generation);

int direct_live_id(struct timeout *ti,
                   struct sanlk_lockspace *ls,
                   uint64_t *timestamp,
                   uint64_t *owner_id,
                   uint64_t *owner_generation,
                   int *live);

int direct_init(struct timeout *ti,
                struct sanlk_lockspace *ls,
                struct sanlk_resource *res,
                int max_hosts, int num_hosts);

int direct_read_leader(struct timeout *ti,
                       struct sanlk_lockspace *ls,
                       struct sanlk_resource *res,
                       struct leader_record *leader_ret);

int direct_dump(struct timeout *ti, char *dump_path);

#endif