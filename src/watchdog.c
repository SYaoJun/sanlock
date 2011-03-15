/*
 * Copyright (C) 2010-2011 Red Hat, Inc.  All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
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
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "sanlock_internal.h"
#include "log.h"
#include "watchdog.h"

/*
 * Purpose of watchdog: to forcibly reset the host in the case where a
 * supervised pid is running but sanlock daemon does not renew its lease
 * and does not kill the pid (or it kills the pid but the pid does not
 * exit).  So, just before the pid begins running with granted leases,
 * /dev/watchdog needs to be armed to reboot the host if things go bad right
 * after the pid goes ahead.
 */

#include "../wdmd/wdmd.h"

static int daemon_wdmd_con;

void update_watchdog_file(struct space *sp, uint64_t timestamp)
{
	int rv;

	if (!options.use_watchdog)
		return;

	rv = wdmd_test_live(sp->wd_fd, timestamp, timestamp + to.host_id_renewal_fail_seconds);
	if (rv < 0)
		log_erros(sp, "wdmd_test_live failed %d", rv);
}

int create_watchdog_file(struct space *sp, uint64_t timestamp)
{
	char name[WDMD_NAME_SIZE];
	int con, rv;

	if (!options.use_watchdog)
		return 0;

	con = wdmd_connect();
	if (con < 0) {
		log_erros(sp, "wdmd connect failed %d", con);
		goto fail;
	}

	memset(name, 0, sizeof(name));

	snprintf(name, WDMD_NAME_SIZE - 1, "sanlock_%s_hostid%llu",
		 sp->space_name, (unsigned long long)sp->host_id);

	rv = wdmd_register(con, name);
	if (rv < 0) {
		log_erros(sp, "wdmd register failed %d", rv);
		goto fail_close;
	}

	rv = wdmd_test_live(con, timestamp, timestamp + to.host_id_renewal_fail_seconds);
	if (rv < 0) {
		log_erros(sp, "wdmd_test_live failed %d", rv);
		goto fail_close;
	}

	sp->wd_fd = con;
	return 0;

 fail_close:
	close(con);
 fail:
	return -1;
}

void unlink_watchdog_file(struct space *sp)
{
	int rv;

	if (!options.use_watchdog)
		return;

	log_space(sp, "wdmd_test_live 0 0 to disable");

	rv = wdmd_test_live(sp->wd_fd, 0, 0);
	if (rv < 0)
		log_erros(sp, "wdmd_test_live failed %d", rv);
}

void close_watchdog_file(struct space *sp)
{
	if (!options.use_watchdog)
		return;

	close(sp->wd_fd);
}

void close_watchdog(void)
{
	if (!options.use_watchdog)
		return;

	wdmd_refcount_clear(daemon_wdmd_con);
	close(daemon_wdmd_con);
}

/* TODO: add wdmd connection as client so poll detects if it fails? */

int setup_watchdog(void)
{
	char name[WDMD_NAME_SIZE];
	int test_interval, fire_timeout;
	uint64_t last_keepalive;
	int con, rv;

	if (!options.use_watchdog)
		return 0;

	memset(name, 0, sizeof(name));

	snprintf(name, WDMD_NAME_SIZE - 1, "%s", "sanlock_daemon");

	con = wdmd_connect();
	if (con < 0) {
		log_error("wdmd connect failed for watchdog handling");
		goto fail;
	}

	rv = wdmd_register(con, name);
	if (rv < 0) {
		log_error("wdmd register failed");
		goto fail_close;
	}

	rv = wdmd_refcount_set(con);
	if (rv < 0) {
		log_error("wdmd refcount failed");
		goto fail_close;
	}

	rv = wdmd_status(con, &test_interval, &fire_timeout, &last_keepalive);
	if (rv < 0) {
		log_error("wdmd status failed");
		goto fail_clear;
	}

	log_debug("wdmd test_interval %d fire_timeout %d last_keepalive %llu",
		  test_interval, fire_timeout,
		  (unsigned long long)last_keepalive);

	if (to.host_id_renewal_fail_seconds + fire_timeout !=
	    to.host_id_timeout_seconds) {
		log_error("invalid timeout settings "
			  "host_id_renewal_fail %d "
			  "fire_timeout %d "
			  "host_id_timeout %d",
			  to.host_id_renewal_fail_seconds,
			  fire_timeout,
			  to.host_id_timeout_seconds);
		goto fail_clear;
	}

	daemon_wdmd_con = con;
	return 0;

 fail_clear:
	wdmd_refcount_clear(con);
 fail_close:
	close(con);
 fail:
	return -1;
}

