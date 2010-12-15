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
#include <pthread.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#define EXTERN
#include "sanlock_internal.h"
#include "diskio.h"
#include "leader.h"
#include "log.h"
#include "paxos_lease.h"
#include "delta_lease.h"
#include "host_id.h"
#include "token_manager.h"
#include "lockfile.h"
#include "client_msg.h"
#include "sanlock_resource.h"
#include "sanlock_admin.h"

/* priorities are LOG_* from syslog.h */
int log_logfile_priority = LOG_ERR;
int log_syslog_priority = LOG_ERR;
int log_stderr_priority = LOG_ERR;

struct client {
	int used;
	int fd;
	int pid;
	int cmd_active;
	int acquire_done;
	int need_setowner;
	int pid_dead;
	pthread_mutex_t mutex;
	void *workfn;
	void *deadfn;
	struct token *tokens[MAX_LEASES];
};

#define CLIENT_NALLOC 32 /* TODO: test using a small value here */
static int client_maxi;
static int client_size = 0;
static struct client *client = NULL;
static struct pollfd *pollfd = NULL;

/* command line actions */
#define ACT_INIT	1
#define ACT_DAEMON	2
#define ACT_COMMAND	3
#define ACT_ACQUIRE	4
#define ACT_RELEASE	5
#define ACT_SHUTDOWN	6
#define ACT_STATUS	7
#define ACT_LOG_DUMP	8
#define ACT_SET_HOST_ID	9
#define ACT_MIGRATE	10

static char command[COMMAND_MAX];
static int cmd_argc;
static char **cmd_argv;
static int external_shutdown;
static int token_id_counter = 1;

struct cmd_args {
	int ci_in;
	int ci_target;
	struct sm_header header;
};

static void client_alloc(void)
{
	int i;

	if (!client) {
		client = malloc(CLIENT_NALLOC * sizeof(struct client));
		pollfd = malloc(CLIENT_NALLOC * sizeof(struct pollfd));
	} else {
		client = realloc(client, (client_size + CLIENT_NALLOC) *
					 sizeof(struct client));
		pollfd = realloc(pollfd, (client_size + CLIENT_NALLOC) *
					 sizeof(struct pollfd));
		if (!pollfd)
			log_error(NULL, "can't alloc for pollfd");
	}
	if (!client || !pollfd)
		log_error(NULL, "can't alloc for client array");

	for (i = client_size; i < client_size + CLIENT_NALLOC; i++) {
		memset(&client[i], 0, sizeof(struct client));
		pthread_mutex_init(&client[i].mutex, NULL);
		client[i].fd = -1;
		pollfd[i].fd = -1;
		pollfd[i].revents = 0;
	}
	client_size += CLIENT_NALLOC;
}

static void client_ignore(int ci)
{
	pollfd[ci].fd = -1;
	pollfd[ci].events = 0;
}

static void client_back(int ci, int fd)
{
	pollfd[ci].fd = fd;
	pollfd[ci].events = POLLIN;
}

static void client_dead(int ci)
{
	close(client[ci].fd);
	client[ci].used = 0;
	memset(&client[ci], 0, sizeof(struct client));
	client[ci].fd = -1;
	pollfd[ci].fd = -1;
	pollfd[ci].events = 0;
}

static int client_add(int fd, void (*workfn)(int ci), void (*deadfn)(int ci))
{
	int i;

	if (!client)
		client_alloc();
 again:
	for (i = 0; i < client_size; i++) {
		if (!client[i].used) {
			client[i].used = 1;
			client[i].workfn = workfn;
			client[i].deadfn = deadfn ? deadfn : client_dead;
			client[i].fd = fd;
			pollfd[i].fd = fd;
			pollfd[i].events = POLLIN;
			if (i > client_maxi)
				client_maxi = i;
			return i;
		}
	}

	client_alloc();
	goto again;
}

static int find_client_pid(int pid)
{
	int i;

	for (i = 0; i < client_size; i++) {
		if (client[i].used && client[i].pid == pid)
			return i;
	}
	return -1;
}

static int get_peer_pid(int fd, int *pid)
{
	struct ucred cred;
	unsigned int cl = sizeof(cred);

	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0)
		return -1;

	*pid = cred.pid;
	return 0;
}

static void client_pid_dead(int ci)
{
	struct client *cl = &client[ci];
	int delay_release = 0;
	int i, pid;

	log_debug(NULL, "client_pid_dead ci %d pid %d", ci, cl->pid);

	/* cmd_acquire_thread may still be waiting for the tokens
	   to be acquired.  if it is, tell it to release them when
	   finished */

	pthread_mutex_lock(&cl->mutex);
	pid = cl->pid;
	cl->pid = -1;
	cl->pid_dead = 1;

	/* TODO: handle other cmds in progress */
	if ((cl->cmd_active == SM_CMD_ACQUIRE) && !cl->acquire_done) {
		delay_release = 1;
		/* client_dead() also delayed */
	}
	pthread_mutex_unlock(&cl->mutex);

	if (pid > 0)
		kill(SIGKILL, pid);

	/* the dead pid may have previously released some resources
	   that are being kept on the saved_resources list in case
	   the pid wanted to reacquire them */

	purge_saved_resources(pid);

	if (delay_release) {
		log_debug(NULL, "client_pid_dead delay release");
		return;
	}

	/* cmd_acquire_thread is done so we can release tokens here */

	for (i = 0; i < MAX_LEASES; i++) {
		if (cl->tokens[i])
			release_token_async(cl->tokens[i]);
	}

	client_dead(ci);
}

static void kill_pids(void)
{
	int ci;

	/* TODO: try killscript first if one is provided */

	for (ci = 0; ci < client_maxi; ci++) {
		if (client[ci].pid)
			kill(SIGTERM, client[ci].pid);
	}

	/* TODO: go back to poll loop in an attempt to clean up some pids
	   from killscript or SIGTERM before calling here again again to
	   use SIGKILL */

	sleep(2);

	for (ci = 0; ci < client_maxi; ci++) {
		if (client[ci].pid)
			kill(SIGTERM, client[ci].pid);
	}
}

static int all_pids_dead(void)
{
	int ci;

	for (ci = 0; ci < client_maxi; ci++) {
		if (client[ci].pid)
			return 0;
	}
	return 1;
}

#define MAIN_POLL_MS 2000

static int main_loop(void)
{
	int poll_timeout = MAIN_POLL_MS;
	void (*workfn) (int ci);
	void (*deadfn) (int ci);
	int i, rv, killing_pids = 0;

	while (1) {
		rv = poll(pollfd, client_maxi + 1, poll_timeout);
		if (rv == -1 && errno == EINTR)
			continue;
		if (rv < 0) {
			/* not sure */
		}
		for (i = 0; i <= client_maxi; i++) {
			if (client[i].fd < 0)
				continue;
			if (pollfd[i].revents & POLLIN) {
				workfn = client[i].workfn;
				if (workfn)
					workfn(i);
			}
			if (pollfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				deadfn = client[i].deadfn;
				if (deadfn)
					deadfn(i);
			}
		}

		if (killing_pids && all_pids_dead())
			break;

		if (external_shutdown || !our_host_id_renewed()) {
			kill_pids();
			killing_pids = 1;
		}
	}

	return 0;
}

static int set_cmd_active(int ci_target, int cmd)
{
	struct client *cl = &client[ci_target];
	int cmd_active = 0;

	pthread_mutex_lock(&cl->mutex);

	/* TODO: find a nicer, more general way to handle this? */
	if (cl->need_setowner && cmd != SM_CMD_SETOWNER) {
		log_error(NULL, "set_cmd_active ci %d cmd %d need_setowner",
			  ci_target, cmd);
		pthread_mutex_unlock(&cl->mutex);
		return -EBUSY;
	}

	cmd_active = cl->cmd_active;

	if (!cmd) {
		/* active to inactive */
		cl->cmd_active = 0;
	} else {
		/* inactive to active */
		if (!cl->cmd_active)
			cl->cmd_active = cmd;
	}
	pthread_mutex_unlock(&cl->mutex);

	if (cmd && cmd_active) {
		log_error(NULL, "set_cmd_active ci %d cmd %d busy %d",
			  ci_target, cmd, cmd_active);
		return -EBUSY;
	}

	if (!cmd && !cmd_active) {
		log_error(NULL, "set_cmd_active ci %d already zero",
			  ci_target);
	}

	return 0;
}

/* clear the unreceived portion of an aborted command */

static void client_recv_all(int ci, struct sm_header *h_recv, int pos)
{
	char trash[64];
	int rem = h_recv->length - sizeof(struct sm_header) - pos;
	int rv, total = 0;

	if (!rem)
		return;

	while (1) {
		rv = recv(client[ci].fd, trash, sizeof(trash), MSG_DONTWAIT);
		if (rv <= 0)
			break;
		total += rv;

		if (total > MAX_CLIENT_MSG)
			break;
	}

	log_debug(NULL, "recv_all ci %d rem %d total %d", ci, rem, total);
}

static void *cmd_acquire_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct client *cl;
	struct sync_disk *disks = NULL;
	struct token *token = NULL;
	struct token *new_tokens[MAX_LEASES];
	struct sanlk_resource res;
	struct sanlk_options opt;
	char *opt_str;
	uint64_t reacquire_lver = 0;
	int fd, rv, i, j, disks_len, num_disks, empty_slots, opened;
	int alloc_count = 0, add_count = 0, open_count = 0, acquire_count = 0;
	int pos = 0, need_setowner = 0, pid_dead = 0;
	int new_tokens_count;

	cl = &client[ca->ci_target];
	fd = client[ca->ci_in].fd;

	log_debug(NULL, "cmd_acquire ci_in %d ci_target %d pid %d",
		  ca->ci_in, ca->ci_target, cl->pid);

	/*
	 * check if we can we add this many new leases
	 */

	new_tokens_count = ca->header.data;
	if (new_tokens_count > MAX_LEASES) {
		log_error(NULL, "cmd_acquire new_tokens_count %d max %d",
			  new_tokens_count, MAX_LEASES);
		rv = -E2BIG;
		goto fail_reply;
	}

	pthread_mutex_lock(&cl->mutex);
	empty_slots = 0;
	for (i = 0; i < MAX_LEASES; i++) {
		if (!cl->tokens[i])
			empty_slots++;
	}
	pthread_mutex_unlock(&cl->mutex);

	if (empty_slots < new_tokens_count) {
		log_error(NULL, "cmd_acquire new_tokens_count %d empty %d",
			  new_tokens_count, empty_slots);
		rv = -ENOSPC;
		goto fail_reply;
	}

	/*
	 * read resource input and allocate tokens for each
	 */

	for (i = 0; i < new_tokens_count; i++) {
		token = malloc(sizeof(struct token));
		if (!token) {
			rv = -ENOMEM;
			goto fail_free;
		}
		memset(token, 0, sizeof(struct token));


		/*
		 * receive sanlk_resource, copy into token
		 */

		rv = recv(fd, &res, sizeof(struct sanlk_resource), MSG_WAITALL);
		if (rv > 0)
			pos += rv;
		if (rv != sizeof(struct sanlk_resource)) {
			log_error(NULL, "cmd_acquire recv %d %d", rv, errno);
			free(token);
			rv = -EIO;
			goto fail_free;
		}

		log_debug(NULL, "cmd_acquire recv res %d %s %d %u %llu", rv,
			  res.name, res.num_disks, res.data32,
			  (unsigned long long)res.data64);
		strncpy(token->resource_name, res.name, SANLK_NAME_LEN);
		token->num_disks = res.num_disks;
		token->acquire_data32 = res.data32;
		token->acquire_data64 = res.data64;


		/*
		 * receive sanlk_disk's / sync_disk's
		 *
		 * WARNING: as a shortcut, this requires that sync_disk and
		 * sanlk_disk match; this is the reason for the pad fields
		 * in sanlk_disk (TODO: let these differ)
		 */

		num_disks = token->num_disks;
		if (num_disks > MAX_DISKS) {
			free(token);
			rv = -ERANGE;
			goto fail_free;
		}

		disks = malloc(num_disks * sizeof(struct sync_disk));
		if (!disks) {
			free(token);
			rv = -ENOMEM;
			goto fail_free;
		}

		disks_len = num_disks * sizeof(struct sync_disk);
		memset(disks, 0, disks_len);

		rv = recv(fd, disks, disks_len, MSG_WAITALL);
		if (rv > 0)
			pos += rv;
		if (rv != disks_len) {
			log_error(NULL, "cmd_acquire recv %d %d", rv, errno);
			free(disks);
			free(token);
			rv = -EIO;
			goto fail_free;
		}
		log_debug(NULL, "cmd_acquire recv disks %d", rv);

		/* zero out pad1 and pad2, see WARNING above */
		for (j = 0; j < num_disks; j++) {
			disks[j].sector_size = 0;
			disks[j].fd = 0;

			log_debug(NULL, "cmd_acquire recv disk %s %llu",
				  disks[j].path,
				  (unsigned long long)disks[j].offset);
		}

		token->token_id = token_id_counter++;
		token->disks = disks;
		new_tokens[i] = token;
		alloc_count++;
	}

	/*
	 * receive per-command sanlk_options and opt string (if any)
	 */

	rv = recv(fd, &opt, sizeof(struct sanlk_options), MSG_WAITALL);
	if (rv > 0)
		pos += rv;
	if (rv != sizeof(struct sanlk_options)) {
		log_error(NULL, "cmd_acquire recv %d %d", rv, errno);
		rv = -EIO;
		goto fail_free;
	}

	log_debug(NULL, "cmd_acquire recv opt %d %x %u", rv, opt.flags, opt.len);

	if (!opt.len)
		goto skip_opt_str;

	opt_str = malloc(opt.len);
	if (!opt_str) {
		rv = -ENOMEM;
		goto fail_free;
	}

	rv = recv(fd, opt_str, opt.len, MSG_WAITALL);
	if (rv > 0)
		pos += rv;
	if (rv != opt.len) {
		log_error(NULL, "cmd_acquire recv %d %d", rv, errno);
		free(opt_str);
		rv = -EIO;
		goto fail_free;
	}

	log_debug(NULL, "cmd_acquire recv str %d", rv);

	/* TODO: warn if header.length != sizeof(header) + pos ? */

	log_debug(NULL, "cmd_acquire command data done %d bytes", pos);


	/*
	 * all command input has been received, start doing the acquire
	 */

 skip_opt_str:
	if (opt.flags & SANLK_FLG_INCOMING)
		need_setowner = 1;

	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];
		rv = add_resource(token, cl->pid);
		if (rv < 0) {
			log_error(token, "cmd_acquire add_resource %d", rv);
			goto fail_del;
		}
		add_count++;
	}

	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];
		opened = open_disks(token->disks, token->num_disks);
		if (!majority_disks(token, opened)) {
			log_error(token, "cmd_acquire open_disks %d", opened);
			rv = -ENODEV;
			goto fail_close;
		}
		open_count++;
	}

	for (i = 0; i < new_tokens_count; i++) {
		token = new_tokens[i];
		if (opt.flags & SANLK_FLG_INCOMING) {
			rv = receive_lease(token, opt_str);
		} else {
			if (opt.flags & SANLK_FLG_REACQUIRE)
				reacquire_lver = token->prev_lver;
			rv = acquire_lease(token, reacquire_lver);
		}
		save_resource_leader(token);

		if (rv < 0) {
			log_error(token, "cmd_acquire lease %d", rv);
			goto fail_release;
		}
		acquire_count++;
	}

	/* 
	 * if pid dead and success|fail, release all
	 * if pid not dead and success, reply
	 * if pid not dead and fail, release new, reply
	 *
	 * transfer all new_tokens into cl->tokens; client_pid_dead
	 * is reponsible from here to release old and new from cl->tokens
	 */

	pthread_mutex_lock(&cl->mutex);

	if (cl->pid_dead) {
		pthread_mutex_unlock(&cl->mutex);
		log_error(NULL, "cmd_acquire pid dead");
		pid_dead = 1;
		rv = -ENOTTY;
		goto fail_dead;
	}

	empty_slots = 0;
	for (i = 0; i < MAX_LEASES; i++) {
		if (!cl->tokens[i])
			empty_slots++;
	}
	if (empty_slots < new_tokens_count) {
		pthread_mutex_unlock(&cl->mutex);
		log_error(NULL, "cmd_acquire new_tokens_count %d slots %d",
			  new_tokens_count, empty_slots);
		rv = -ENOSPC;
		goto fail_release;
	}
	for (i = 0; i < new_tokens_count; i++) {
		for (j = 0; j < MAX_LEASES; j++) {
			if (!cl->tokens[j]) {
				cl->tokens[j] = new_tokens[i];
				break;
			}
		}
	}

	cl->acquire_done = 1;
	cl->cmd_active = 0;   /* instead of set_cmd_active(0) */
	cl->need_setowner = need_setowner;
	pthread_mutex_unlock(&cl->mutex);

	log_debug(NULL, "cmd_acquire done %d", new_tokens_count);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = new_tokens_count;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;

 fail_dead:
	/* clear out all the old tokens */
	for (i = 0; i < MAX_LEASES; i++) {
		token = cl->tokens[i];
		if (!token)
			continue;
		release_lease(token);
		close_disks(token->disks, token->num_disks);
		del_resource(token);
		free_token(token);
	}

 fail_release:
	for (i = 0; i < acquire_count; i++)
		release_lease(new_tokens[i]);

 fail_close:
	for (i = 0; i < open_count; i++)
		close_disks(new_tokens[i]->disks, new_tokens[i]->num_disks);

 fail_del:
	for (i = 0; i < add_count; i++)
		del_resource(new_tokens[i]);

 fail_free:
	for (i = 0; i < alloc_count; i++)
		free_token(new_tokens[i]);

 fail_reply:
	set_cmd_active(ca->ci_target, 0);

	client_recv_all(ca->ci_in, &ca->header, pos);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = rv;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);

	if (pid_dead)
		client_dead(ca->ci_target);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

static void *cmd_release_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct token *token;
	struct sanlk_resource res;
	int results[MAX_LEASES];
	struct client *cl;
	int fd, rv, i, j, found, rem_tokens_count;

	cl = &client[ca->ci_target];
	fd = client[ca->ci_in].fd;

	log_debug(NULL, "cmd_release ci_in %d ci_target %d pid %d",
		  ca->ci_in, ca->ci_target, cl->pid);

	memset(results, 0, sizeof(results));
	rem_tokens_count = ca->header.data;

	for (i = 0; i < rem_tokens_count; i++) {
		rv = recv(fd, &res, sizeof(struct sanlk_resource), MSG_WAITALL);
		if (rv != sizeof(struct sanlk_resource)) {
			log_error(NULL, "cmd_release recv fd %d %d %d", fd, rv, errno);
			results[i] = -1;
			break;
		}

		found = 0;

		for (j = 0; j < MAX_LEASES; j++) {
			token = cl->tokens[j];
			if (!token)
				continue;

			if (memcmp(token->resource_name, res.name, NAME_ID_SIZE))
				continue;

			rv = release_lease(token);
			save_resource(token);
			free_token(token);
			cl->tokens[j] = NULL;
			results[i] = rv;
			found = 1;
			break;
		}

		if (!found) {
			log_error(NULL, "cmd_release pid %d no resource %s",
				  cl->pid, res.name);
			results[i] = -ENOENT;
		}
	}

	set_cmd_active(ca->ci_target, 0);

	log_debug(NULL, "cmd_release done %d", rem_tokens_count);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h) + sizeof(int) * rem_tokens_count;
	send(fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);
	send(fd, &results, sizeof(int) * rem_tokens_count, MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

/* 
 * TODO: what if cl->pid fails during migrate?
 *       what if ci_reply fails?
 */

static void *cmd_migrate_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct token *token;
	struct token *tokens_reply;
	struct client *cl;
	uint64_t target_host_id = 0;
	int fd, rv, i, tokens_len, result = 0, total = 0, total2 = 0;

	cl = &client[ca->ci_target];
	fd = client[ca->ci_in].fd;

	log_debug(NULL, "cmd_migrate ci_in %d ci_target %d pid %d",
		  ca->ci_in, ca->ci_target, cl->pid);

	rv = recv(fd, &target_host_id, sizeof(uint64_t), MSG_WAITALL);
	if (rv != sizeof(uint64_t)) {
		result = -EIO;
		goto reply;
	}

	if (!target_host_id) {
		result = -EINVAL;
		goto reply;
	}

	for (i = 0; i < MAX_LEASES; i++) {
		if (cl->tokens[i])
			total++;
	}

	tokens_len = total * sizeof(struct token);
	tokens_reply = malloc(tokens_len);
	if (!tokens_reply) {
		result = -ENOMEM;
		total = 0;
		goto reply;
	}
	memset(tokens_reply, 0, tokens_len);

	for (i = 0; i < MAX_LEASES; i++) {
		token = cl->tokens[i];
		if (!token)
			continue;

		rv = migrate_lease(token, target_host_id);
		if (rv < 0 && !result)
			result = rv;

		/* TODO: would it be better to quit after one failure? */

		if (total2 == total) {
			log_error(NULL, "cmd_migrate total %d changed", total);
			continue;
		}

		memcpy(&tokens_reply[total2++], token, sizeof(struct token));
	}

 reply:
	/* TODO: for success I don't think we want to clear cmd_active
	   here.  We probably want to wait until the migrate is done
	   and then do set_cmd_active(0)? */

	if (result < 0)
		set_cmd_active(ca->ci_target, 0);

	log_debug(NULL, "cmd_migrate done %d", total);

	/* TODO: encode tokens_reply as a string to send back */

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h) + tokens_len;
	h.data = result;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);
	if (total)
		send(fd, tokens_reply, tokens_len, MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

/* become the full owner of leases that were migrated to us;
   go through each of the pid's tokens, set next_owner_id for each,
   then reply to client with result */

static void *cmd_setowner_thread(void *args_in)
{
	struct cmd_args *ca = args_in;
	struct sm_header h;
	struct token *token;
	struct token *tokens_reply;
	struct client *cl;
	int result = 0, total = 0, total2 = 0;
	int fd, rv, i, tokens_len;

	cl = &client[ca->ci_target];
	fd = client[ca->ci_in].fd;

	log_debug(NULL, "cmd_setowner ci_in %d ci_target %d pid %d",
		  ca->ci_in, ca->ci_target, cl->pid);

	for (i = 0; i < MAX_LEASES; i++) {
		if (cl->tokens[i])
			total++;
	}

	tokens_len = total * sizeof(struct token);
	tokens_reply = malloc(tokens_len);
	if (!tokens_reply) {
		result = -ENOMEM;
		total = 0;
		goto reply;
	}
	memset(tokens_reply, 0, tokens_len);

	for (i = 0; i < MAX_LEASES; i++) {
		token = cl->tokens[i];
		if (!token)
			continue;

		rv = setowner_lease(token);
		if (rv < 0)
			result = -1;

		if (total2 == total) {
			log_error(NULL, "cmd_setowner total %d changed", total);
			continue;
		}

		memcpy(&tokens_reply[total2++], token, sizeof(struct token));
	}

 reply:
	set_cmd_active(ca->ci_target, 0);

	log_debug(NULL, "cmd_setowner done %d", total);

	memcpy(&h, &ca->header, sizeof(struct sm_header));
	h.length = sizeof(h) + tokens_len;
	h.data = result;
	send(fd, &h, sizeof(h), MSG_NOSIGNAL);
	if (total)
		send(fd, tokens_reply, tokens_len, MSG_NOSIGNAL);

	client_back(ca->ci_in, fd);
	free(ca);
	return NULL;
}

static void cmd_status(int fd, struct sm_header *h_recv)
{
	send(fd, h_recv, sizeof(struct sm_header), MSG_NOSIGNAL);
}

static void cmd_log_dump(int fd, struct sm_header *h_recv)
{
	struct sm_header h;

	memcpy(&h, h_recv, sizeof(struct sm_header));

	/* can't send header until taking log_mutex to find the length */

	write_log_dump(fd, &h);
}

static void cmd_set_host_id(int fd, struct sm_header *h_recv)
{
	struct sm_header h;
	int rv;

	if (options.our_host_id == h_recv->data) {
		rv = 0;
		goto reply;
	}

	if (options.our_host_id > 0) {
		log_error(NULL, "cmd_set_host_id our_host_id already set %d",
			  options.our_host_id);
		rv = 1;
		goto reply;
	}

	if (!h_recv->data) {
		log_error(NULL, "cmd_set_host_id invalid host_id %d",
			  h_recv->data);
		rv = 1;
		goto reply;
	}

	options.our_host_id = h_recv->data;

	log_debug(NULL, "set_host_id %d", options.our_host_id);

	rv = start_host_id();
	if (rv < 0) {
		log_error(NULL, "cmd_set_host_id start_host_id %d error %d",
			  options.our_host_id, rv);
		options.our_host_id = -1;
	}

 reply:
	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = rv;
	send(fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);
}

static void process_cmd_thread(int ci_in, struct sm_header *h_recv)
{
	pthread_t cmd_thread;
	pthread_attr_t attr;
	struct cmd_args *ca;
	struct sm_header h;
	int rv, ci_target;

	if (h_recv->data2 != -1) {
		/* lease for another registered client with pid specified by data2 */
		ci_target = find_client_pid(h_recv->data2);
		if (ci_target < 0) {
			rv = -ENOENT;
			goto fail;
		}
	} else {
		/* lease for this registered client */
		ci_target = ci_in;
	}

	/* the target client must be registered */

	if (client[ci_target].pid <= 0) {
		rv = -EPERM;
		goto fail;
	}

	rv = set_cmd_active(ci_target, h_recv->cmd);
	if (rv < 0)
		goto fail;

	ca = malloc(sizeof(struct cmd_args));
	if (!ca) {
		rv = -ENOMEM;
		goto fail_active;
	}
	ca->ci_in = ci_in;
	ca->ci_target = ci_target;
	memcpy(&ca->header, h_recv, sizeof(struct sm_header));

	/* TODO: use a thread pool? */

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	switch (h_recv->cmd) {
	case SM_CMD_ACQUIRE:
		rv = pthread_create(&cmd_thread, &attr, cmd_acquire_thread, ca);
		break;
	case SM_CMD_RELEASE:
		rv = pthread_create(&cmd_thread, &attr, cmd_release_thread, ca);
		break;
	case SM_CMD_MIGRATE:
		rv = pthread_create(&cmd_thread, &attr, cmd_migrate_thread, ca);
		break;
	case SM_CMD_SETOWNER:
		rv = pthread_create(&cmd_thread, &attr, cmd_setowner_thread, ca);
		break;
	};

	pthread_attr_destroy(&attr);
	if (rv < 0) {
		log_error(NULL, "create cmd thread failed");
		goto fail_free;
	}

	return;

 fail_free:
	free(ca);
 fail_active:
	set_cmd_active(ci_target, 0);
 fail:
	client_recv_all(ci_in, h_recv, 0);

	memcpy(&h, h_recv, sizeof(struct sm_header));
	h.length = sizeof(h);
	h.data = rv;
	send(client[ci_in].fd, &h, sizeof(struct sm_header), MSG_NOSIGNAL);
	client_back(ci_in, client[ci_in].fd);
}

static void process_cmd_daemon(int ci, struct sm_header *h_recv)
{
	int rv, pid, auto_close = 1;
	int fd = client[ci].fd;

	switch (h_recv->cmd) {
	case SM_CMD_REGISTER:
		rv = get_peer_pid(fd, &pid);
		if (rv < 0)
			break;
		log_debug(NULL, "cmd_register ci %d fd %d pid %d", ci, fd, pid);
		client[ci].pid = pid;
		client[ci].deadfn = client_pid_dead;
		auto_close = 0;
		break;
	case SM_CMD_SHUTDOWN:
		external_shutdown = 1;
		break;
	case SM_CMD_STATUS:
		cmd_status(fd, h_recv);
		break;
	case SM_CMD_LOG_DUMP:
		cmd_log_dump(fd, h_recv);
		break;
	case SM_CMD_SET_HOST_ID:
		cmd_set_host_id(fd, h_recv);
		break;
	};

	if (auto_close)
		close(fd);
}

static void process_connection(int ci)
{
	struct sm_header h;
	void (*deadfn)(int ci);
	int rv;

	memset(&h, 0, sizeof(h));

	rv = recv(client[ci].fd, &h, sizeof(h), MSG_WAITALL);
	if (!rv)
		return;
	if (rv < 0) {
		log_error(NULL, "ci %d recv error %d", ci, errno);
		return;
	}
	if (rv != sizeof(h)) {
		log_error(NULL, "ci %d recv size %d", ci, rv);
		goto dead;
	}
	if (h.magic != SM_MAGIC) {
		log_error(NULL, "ci %d recv %d magic %x", ci, rv, h.magic);
		goto dead;
	}
	if ((options.our_host_id < 0) && (h.cmd != SM_CMD_SET_HOST_ID)) {
		log_error(NULL, "host_id not set");
		goto dead;
	}

	switch (h.cmd) {
	case SM_CMD_REGISTER:
	case SM_CMD_SHUTDOWN:
	case SM_CMD_STATUS:
	case SM_CMD_LOG_DUMP:
	case SM_CMD_SET_HOST_ID:
		process_cmd_daemon(ci, &h);
		break;
	case SM_CMD_ACQUIRE:
	case SM_CMD_RELEASE:
	case SM_CMD_MIGRATE:
	case SM_CMD_SETOWNER:
		client_ignore(ci);
		process_cmd_thread(ci, &h);
		break;
	default:
		log_error(NULL, "ci %d cmd %d unknown", ci, h.cmd);
	};

	return;

 dead:
	deadfn = client[ci].deadfn;
	if (deadfn)
		deadfn(ci);
}

static void process_listener(int ci GNUC_UNUSED)
{
	int fd;
	int on = 1;

	fd = accept(client[ci].fd, NULL, NULL);
	if (fd < 0)
		return;

	setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	client_add(fd, process_connection, NULL);
}

static int setup_listener(void)
{
	int rv, fd;

	rv = setup_listener_socket(&fd);
	if (rv < 0)
		return rv;

	client_add(fd, process_listener, NULL);
	return 0;
}

static void sigterm_handler(int sig GNUC_UNUSED)
{
	external_shutdown = 1;
}

static int make_dirs(void)
{
	mode_t old_umask;
	int rv;

	old_umask = umask(0022);
	rv = mkdir(SANLK_RUN_DIR, 0777);
	if (rv < 0 && errno != EEXIST)
		goto out;

	rv = 0;
 out:
	umask(old_umask);
	return rv;
}

static int do_daemon(void)
{
	struct sigaction act;
	int fd, rv;

	/* TODO: copy comprehensive daemonization method from libvirtd */

	if (!options.no_daemon_fork) {
		if (daemon(0, 0) < 0) {
			log_tool("cannot fork daemon\n");
			exit(EXIT_FAILURE);
		}
		umask(0);
	}

	memset(&act, 0, sizeof(act));
	act.sa_handler = sigterm_handler;
	rv = sigaction(SIGTERM, &act, NULL);
	if (rv < 0)
		return -rv;

	/*
	 * after creating dirs and setting up logging the daemon can
	 * use log_error/log_debug
	 */

	rv = make_dirs();
	if (rv < 0) {
		log_tool("cannot create logging dirs\n");
		return -1;
	}

	setup_logging();

	fd = lockfile(NULL, SANLK_RUN_DIR, SANLK_LOCKFILE_NAME);
	if (fd < 0)
		goto out;

	rv = setup_listener();
	if (rv < 0)
		goto out_lockfile;

	setup_token_manager();
	if (rv < 0)
		goto out_lockfile;

	if (options.our_host_id > 0) {
		rv = start_host_id();
		if (rv < 0) {
			log_error(NULL, "start_host_id %d error %d",
				  options.our_host_id, rv);
			options.our_host_id = -1;
			goto out_token;
		}
	}

	main_loop();

	stop_host_id();
 out_token:
	close_token_manager();
 out_lockfile:
	unlink_lockfile(fd, SANLK_RUN_DIR, SANLK_LOCKFILE_NAME);
 out:
	close_logging();
	return rv;
}

static int do_init(void)
{
	struct sanlk_resource *res;
	struct token *token;
	struct sync_disk sd;
	int num_opened;
	int i, j, rv = 0;

	if (!options.host_id_path[0])
		goto tokens;

	memset(&sd, 0, sizeof(struct sync_disk));
	strncpy(sd.path, options.host_id_path, DISK_PATH_LEN);
	sd.offset = options.host_id_offset;

	num_opened = open_disks(&sd, 1);
	if (num_opened != 1) {
		log_tool("cannot open disk %s", sd.path);
		return -1;
	}

	rv = delta_lease_init(&sd, com.num_hosts, com.max_hosts);
	if (rv < 0) {
		log_tool("cannot initialize host_id disk");
		return -1;
	}

 tokens:
	for (i = 0; i < com.res_count; i++) {
		res = com.res_args[i];

		rv = create_token(res->num_disks, &token);
		if (rv < 0)
			return rv;

		strncpy(token->resource_name, res->name, NAME_ID_SIZE);

		/* see WARNING above about sync_disk == sanlk_disk */

		memcpy(token->disks, &res->disks,
		       token->num_disks * sizeof(struct sync_disk));

		/* zero out pad1 and pad2, see WARNING above */
		for (j = 0; j < token->num_disks; j++) {
			token->disks[j].sector_size = 0;
			token->disks[j].fd = 0;
		}

		num_opened = open_disks(token->disks, token->num_disks);
		if (!majority_disks(token, num_opened)) {
			log_tool("cannot open majority of disks");
			return -1;
		}

		rv = paxos_lease_init(token, com.num_hosts, com.max_hosts);
		if (rv < 0) {
			log_tool("cannot initialize disks");
			return -1;
		}

		free_token(token);
	}

	return 0;
}

static void print_usage(void)
{
	printf("Usage:\n");
	printf("sanlock <action> [options]\n\n");
	printf("main actions:\n");
	printf("  help			print usage\n");
	printf("  init			initialize disk areas for host_id and resource leases\n");
	printf("  daemon		start daemon\n");
	printf("\n");
	printf("client actions:\n");
	printf("  command		ask daemon to acquire leases, then run command\n");
	printf("  acquire		ask daemon to acquire leases for another pid\n");
	printf("  release		ask daemon to release leases for another pid\n");
	printf("  status		print internal daemon state\n");
	printf("  log_dump		print internal daemon debug buffer\n");
	printf("  shutdown		ask daemon to kill pids, release leases and exit\n");
	printf("  set_host_id		set daemon host_id and host_id lease area\n");
	printf("\n");

	printf("\ninit [options] -h <num_hosts> -l LEASE\n");
	printf("  -h <num_hosts>	max host id that will be able to acquire the lease\n");
	printf("  -H <max_hosts>	max number of hosts the disk area will support\n");
	printf("                        (default %d)\n", DEFAULT_MAX_HOSTS);
	printf("  -m <num>		cluster mode of hosts (default 0)\n");
	printf("  -d <path>		disk path for host_id leases\n");
	printf("  -o <num>		offset on disk for host_id leases\n");
	printf("  -l LEASE		resource lease description, see below\n");

	printf("\ndaemon [options]\n");
	printf("  -D			don't fork and print all logging to stderr\n");
	printf("  -L <level>		write logging at level and up to logfile (-1 none)\n");
	printf("  -S <level>		write logging at level and up to syslog (-1 none)\n");
	printf("  -m <num>		cluster mode of hosts (default 0)\n");
	printf("  -w <num>		enable (1) or disable (0) writing watchdog files\n");
	printf("  -a <num>		use async io (1 yes, 0 no)\n");
	printf("  -i <num>		local host_id\n");
	printf("  -d <path>		disk path for host_id leases\n");
	printf("  -o <num>		offset on disk for host_id leases\n");
	printf("  -s <key=n,key=n,...>	change default timeouts in seconds, key (default):\n");
	printf("                        io_timeout (%d)\n", DEFAULT_IO_TIMEOUT_SECONDS);
	printf("                        host_id_renewal (%d)\n", DEFAULT_HOST_ID_RENEWAL_SECONDS);
	printf("                        host_id_renewal_fail (%d)\n", DEFAULT_HOST_ID_RENEWAL_FAIL_SECONDS);
	printf("                        host_id_timeout (%d)\n", DEFAULT_HOST_ID_TIMEOUT_SECONDS);

	printf("\nset_host_id [options]\n");
	printf("  -i <num>		local host_id\n");
	printf("  -d <path>		disk path for host_id leases\n");
	printf("  -o <num>		offset on disk for host_id leases\n");

	printf("\ncommand -l LEASE -c <path> <args>\n");
	printf("  -l LEASE		resource lease description, see below\n");
	printf("  -c <path> <args>	run command with args, -c must be final option\n");

	printf("\nacquire -p <pid> -l LEASE\n");
	printf("  -p <pid>		process that lease should be added for\n");
	printf("  -l LEASE		resource lease description, see below\n");

	printf("\nrelease -p <pid> -r <resource_name>\n");
	printf("  -p <pid>		process whose lease should be released\n");
	printf("  -r <resource_name>	resource name of a previously acquired lease\n");

	printf("\nstatus\n");

	printf("\nlog_dump\n");

	printf("\nshutdown\n");

	printf("\nLEASE = <resource_name>:<path>:<offset>[:<path>:<offset>...]\n");
	printf("  <resource_name>	name of resource being leased\n");
	printf("  <path>		disk path\n");
	printf("  <offset>[s|KB|MB]	offset on disk, default unit bytes\n");
	printf("                        [s = sectors, KB = 1024 bytes, MB = 1024 KB]\n");
	printf("  [:<path>:<offset>...] other disks in a multi-disk lease\n");
	printf("\n");
}

static int create_sanlk_resource(int num_disks, struct sanlk_resource **res_out)
{
	struct sanlk_resource *res;
	int len;

	len = sizeof(struct sanlk_resource) +
	      num_disks * sizeof(struct sanlk_disk);

	res = malloc(sizeof(struct sanlk_resource) +
		     (num_disks * sizeof(struct sanlk_disk)));
	if (!res)
		return -ENOMEM;
	memset(res, 0, len);

	res->num_disks = num_disks;
	*res_out = res;
        return 0;
}

static int add_res_name_to_com(char *arg)
{
	struct sanlk_resource *res;
	int rv;

	if (com.res_count >= MAX_LEASES) {
		log_tool("lease args over max %d", MAX_LEASES);
		return -1;
	}

	rv = create_sanlk_resource(0, &res);
	if (rv < 0) {
		log_tool("resource arg create");
		return rv;
	}

	strncpy(res->name, arg, NAME_ID_SIZE);
	com.res_args[com.res_count] = res;
	com.res_count++;
	return rv;
}

/* arg = <resource_name>:<path>:<offset>[:<path>:<offset>...] */

static int add_lease_to_com(char *arg)
{
	struct sanlk_resource *res;
	char sub[DISK_PATH_LEN + 1];
	char unit[DISK_PATH_LEN + 1];
	int sub_count;
	int colons;
	int num_disks;
	int rv, i, j, d;
	int len = strlen(arg);

	if (com.res_count >= MAX_LEASES) {
		log_tool("lease args over max %d", MAX_LEASES);
		return -1;
	}

	colons = 0;
	for (i = 0; i < strlen(arg); i++) {
		if (arg[i] == '\\') {
			i++;
			continue;
		}

		if (arg[i] == ':')
			colons++;
	}
	if (!colons || (colons % 2)) {
		log_tool("invalid lease arg");
		return -1;
	}
	num_disks = colons / 2;

	if (num_disks > MAX_DISKS) {
		log_tool("invalid lease arg num_disks %d", num_disks);
		return -1;
	}

	rv = create_sanlk_resource(num_disks, &res);
	if (rv < 0) {
		log_tool("lease arg create num_disks %d", num_disks);
		return rv;
	}

	com.res_args[com.res_count] = res;
	com.res_count++;

	d = 0;
	sub_count = 0;
	j = 0;
	memset(sub, 0, sizeof(sub));

	for (i = 0; i < len + 1; i++) {
		if (arg[i] == '\\') {
			if (i == (len - 1)) {
				log_tool("Invalid lease string");
				goto fail;
			}

			i++;
			sub[j++] = arg[i];
			continue;
		}
		if (i < len && arg[i] != ':') {
			if (j >= DISK_PATH_LEN) {
				log_tool("lease arg length error");
				goto fail;
			}
			sub[j++] = arg[i];
			continue;
		}

		/* do something with sub when we hit ':' or end of arg,
		   first sub is id, odd sub is path, even sub is offset */

		if (!sub_count) {
			if (strlen(sub) > NAME_ID_SIZE) {
				log_tool("lease arg id length error");
				goto fail;
			}
			strncpy(res->name, sub, NAME_ID_SIZE);
		} else if (sub_count % 2) {
			if (strlen(sub) > DISK_PATH_LEN-1 || strlen(sub) < 1) {
				log_tool("lease arg path length error");
				goto fail;
			}
			strncpy(res->disks[d].path, sub, DISK_PATH_LEN - 1);
		} else {
			memset(&unit, 0, sizeof(unit));
			rv = sscanf(sub, "%llu%s", (unsigned long long *)&res->disks[d].offset, unit);
			if (!rv || rv > 2) {
				log_tool("lease arg offset error");
				goto fail;
			}
			if (rv > 1) {
				if (unit[0] == 's')
					res->disks[d].units = SANLK_UNITS_SECTORS;
				else if (unit[0] == 'K' && unit[1] == 'B')
					res->disks[d].units = SANLK_UNITS_KB;
				else if (unit[0] == 'M' && unit[1] == 'B')
					res->disks[d].units = SANLK_UNITS_MB;
				else {
					log_tool("unit unknkown: %s", unit);
					goto fail;
				}
			}
			d++;
		}

		sub_count++;
		j = 0;
		memset(sub, 0, sizeof(sub));
	}

	return 0;

 fail:
	free(res);
	return -1;
}

static void set_timeout(char *key, char *val)
{
	if (!strcmp(key, "io_timeout")) {
		to.io_timeout_seconds = atoi(val);
		log_debug(NULL, "io_timeout_seconds %d", to.io_timeout_seconds);
		return;
	}

	if (!strcmp(key, "host_id_timeout")) {
		to.host_id_timeout_seconds = atoi(val);
		log_debug(NULL, "host_id_timeout_seconds %d", to.host_id_timeout_seconds);
		return;
	}

	if (!strcmp(key, "host_id_renewal")) {
		to.host_id_renewal_seconds = atoi(val);
		log_debug(NULL, "host_id_renewal_seconds %d", to.host_id_renewal_seconds);
		return;
	}

	if (!strcmp(key, "host_id_renewal_fail")) {
		to.host_id_renewal_fail_seconds = atoi(val);
		log_debug(NULL, "host_id_renewal_fail_seconds %d", to.host_id_renewal_fail_seconds);
		return;
	}
}

static void parse_timeouts(char *optstr)
{
	int copy_key, copy_val, i, kvi;
	char key[64], val[64];

	copy_key = 1;
	copy_val = 0;
	kvi = 0;

	for (i = 0; i < strlen(optstr); i++) {
		if (optstr[i] == ',') {
			set_timeout(key, val);
			memset(key, 0, sizeof(key));
			memset(val, 0, sizeof(val));
			copy_key = 1;
			copy_val = 0;
			kvi = 0;
			continue;
		}

		if (optstr[i] == '=') {
			copy_key = 0;
			copy_val = 1;
			kvi = 0;
			continue;
		}

		if (copy_key)
			key[kvi++] = optstr[i];
		else if (copy_val)
			val[kvi++] = optstr[i];

		if (kvi > 62) {
			log_error(NULL, "invalid timeout parameter");
			return;
		}
	}

	set_timeout(key, val);
}

#define RELEASE_VERSION "0.0"

static int read_command_line(int argc, char *argv[])
{
	char optchar;
	char *optionarg;
	char *p;
	char *arg1 = argv[1];
	int optionarg_used;
	int i, j, rv, len, begin_command = 0;

	if (argc < 2 || !strcmp(arg1, "help") || !strcmp(arg1, "--help") ||
	    !strcmp(arg1, "-h")) {
		print_usage();
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "version") || !strcmp(arg1, "--version") ||
	    !strcmp(arg1, "-V")) {
		printf("%s %s (built %s %s)\n",
		       argv[0], RELEASE_VERSION, __DATE__, __TIME__);
		exit(EXIT_SUCCESS);
	}

	if (!strcmp(arg1, "init"))
		com.action = ACT_INIT;
	else if (!strcmp(arg1, "daemon"))
		com.action = ACT_DAEMON;
	else if (!strcmp(arg1, "command"))
		com.action = ACT_COMMAND;
	else if (!strcmp(arg1, "acquire"))
		com.action = ACT_ACQUIRE;
	else if (!strcmp(arg1, "release"))
		com.action = ACT_RELEASE;
	else if (!strcmp(arg1, "migrate"))
		com.action = ACT_MIGRATE;
	else if (!strcmp(arg1, "shutdown"))
		com.action = ACT_SHUTDOWN;
	else if (!strcmp(arg1, "status"))
		com.action = ACT_STATUS;
	else if (!strcmp(arg1, "log_dump"))
		com.action = ACT_LOG_DUMP;
	else if (!strcmp(arg1, "set_host_id"))
		com.action = ACT_SET_HOST_ID;
	else {
		log_tool("first arg is unknown action");
		print_usage();
		exit(EXIT_FAILURE);
	}

	for (i = 2; i < argc; ) {
		p = argv[i];

		if ((p[0] != '-') || (strlen(p) != 2)) {
			log_tool("unknown option %s", p);
			log_tool("space required before option value");
			print_usage();
			exit(EXIT_FAILURE);
		}

		optchar = p[1];
		i++;

		optionarg = argv[i];
		optionarg_used = 1;

		switch (optchar) {
		case 'D':
			options.no_daemon_fork = 1;
			log_stderr_priority = LOG_DEBUG;
			optionarg_used = 0;
			break;
		case 'L':
			log_logfile_priority = atoi(optionarg);
			break;
		case 'S':
			log_syslog_priority = atoi(optionarg);
			break;
		case 'h':
			com.num_hosts = atoi(optionarg);
			break;
		case 'H':
			com.max_hosts = atoi(optionarg);
			break;
		case 'm':
			options.cluster_mode = atoi(optionarg);
			break;
		case 's':
			parse_timeouts(optionarg);
			break;
		case 'i':
			options.our_host_id = atoi(optionarg);
			break;
		case 'd':
			strncpy(options.host_id_path, optionarg, DISK_PATH_LEN);
			break;
		case 'o':
			options.host_id_offset = atoi(optionarg);
			break;
		case 'a':
			options.use_aio = atoi(optionarg);
			break;
		case 'r':
			if (com.action != ACT_RELEASE)
				return -1;

			rv = add_res_name_to_com(optionarg);
			if (rv < 0)
				return rv;
			break;
		case 'p':
			com.pid = atoi(optionarg);
			break;
		case 't':
			com.host_id = atoi(optionarg);
			break;
		case 'f':
			com.incoming = atoi(optionarg);
			break;
		case 'l':
			if (com.action == ACT_RELEASE)
				return -1;

			rv = add_lease_to_com(optionarg);
			if (rv < 0)
				return rv;
			break;
		case 'w':
			options.use_watchdog = atoi(optionarg);
			break;
		case 'c':
			begin_command = 1;
			optionarg_used = 0;
			break;
		default:
			log_tool("unknown option: %c", optchar);
			exit(EXIT_FAILURE);
		};

		if (optionarg_used)
			i++;

		if (begin_command)
			break;
	}

	/*
	 * the remaining args are for the command
	 *
	 * sanlock -r foo -n 2 -d bar:0 -c /bin/cmd -X -Y -Z
	 * argc = 12
	 * loop above breaks with i = 8, argv[8] = "/bin/cmd"
	 *
	 * cmd_argc = 4 = argc (12) - i (8)
	 * cmd_argv[0] = "/bin/cmd"
	 * cmd_argv[1] = "-X"
	 * cmd_argv[2] = "-Y"
	 * cmd_argv[3] = "-Z"
	 * cmd_argv[4] = NULL (required by execv)
	 */

	if (begin_command) {
		cmd_argc = argc - i;

		if (cmd_argc < 1) {
			log_tool("command option (-c) requires an arg");
			return -EINVAL;
		}

		len = (cmd_argc + 1) * sizeof(char *); /* +1 for final NULL */
		cmd_argv = malloc(len);
		if (!cmd_argv)
			return -ENOMEM;
		memset(cmd_argv, 0, len);

		for (j = 0; j < cmd_argc; j++) {
			cmd_argv[j] = strdup(argv[i++]);
			if (!cmd_argv[j])
				return -ENOMEM;
		}

		strncpy(command, cmd_argv[0], COMMAND_MAX - 1);
	}

	return 0;
}

static void exec_command(void)
{
	if (!command[0]) {
		while (1)
			sleep(10);
	}

	execv(command, cmd_argv);
	perror("execv failed");
}

int main(int argc, char *argv[])
{
	int rv, fd;
	
	memset(&com, 0, sizeof(com));
	com.max_hosts = DEFAULT_MAX_HOSTS;
	com.pid = -1;

	memset(&options, 0, sizeof(options));
	options.use_aio = 1;
	options.use_watchdog = 1;
	options.our_host_id = -1;

	memset(&to, 0, sizeof(to));
	to.io_timeout_seconds = DEFAULT_IO_TIMEOUT_SECONDS;
	to.host_id_renewal_seconds = DEFAULT_HOST_ID_RENEWAL_SECONDS;
	to.host_id_renewal_fail_seconds = DEFAULT_HOST_ID_RENEWAL_FAIL_SECONDS;
	to.host_id_timeout_seconds = DEFAULT_HOST_ID_TIMEOUT_SECONDS;

	rv = read_command_line(argc, argv);
	if (rv < 0)
		goto out;

	switch (com.action) {
	case ACT_DAEMON:
		rv = do_daemon();
		break;

	case ACT_INIT:
		rv = do_init();
		break;

	/* client actions that ask daemon to do something.
	   we could split these into a separate command line
	   utility (note that the lease arg processing is shared
	   between init and acquire.  It would also be a pain
	   to move init into a separate utility because it shares
	   disk paxos code with the daemon. */

	case ACT_COMMAND:
		log_tool("register");
		fd = sanlock_register();
		if (fd < 0)
			goto out;
		log_tool("acquire_self %d resources", com.res_count);
		rv = sanlock_acquire_self(fd, com.res_count, com.res_args, NULL);
		if (rv < 0)
			goto out;
		log_tool("exec_command");
		exec_command();

		/* release happens automatically when pid exits and
		   daemon detects POLLHUP on registered connection */
		break;

	case ACT_ACQUIRE:
		log_tool("acquire_pid %d %d resources", com.pid, com.res_count);
		rv = sanlock_acquire_pid(com.pid, com.res_count, com.res_args, NULL);
		break;

	case ACT_RELEASE:
		log_tool("release_pid %d %d resources", com.pid, com.res_count);
		rv = sanlock_release_pid(com.pid, com.res_count, com.res_args);
		break;

	case ACT_MIGRATE:
		log_tool("migrate_pid %d to host_id %d", com.pid, com.host_id);
		rv = sanlock_migrate_pid(com.pid, com.host_id);
		break;

	case ACT_SHUTDOWN:
		rv = sanlock_shutdown();
		break;

	case ACT_STATUS:
		rv = sanlock_status();
		break;

	case ACT_LOG_DUMP:
		rv = sanlock_log_dump();
		break;

	case ACT_SET_HOST_ID:
		rv = sanlock_set_host_id(options.our_host_id);
		break;

	default:
		break;
	}
 out:
	return rv;
}

