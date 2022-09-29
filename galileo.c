/*
 * Copyright (c) 2022 Omar Polo <op@omarpolo.com>
 * Copyright (c) 2014 Reyk Floeter <reyk@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <event.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <imsg.h>

#include "log.h"
#include "proc.h"
#include "xmalloc.h"

#include "galileo.h"

static int		parent_configure(struct galileo *);
static void		parent_configure_done(struct galileo *);
static void		parent_reload(struct galileo *);
static void		parent_sig_handler(int, short, void *);
static int		parent_dispatch_proxy(int, struct privsep_proc *,
			    struct imsg *);
static __dead void	parent_shutdown(struct galileo *);

static struct privsep_proc procs[] = {
	{ "proxy",	PROC_PROXY, parent_dispatch_proxy, proxy },
};

int privsep_process;

const char *conffile = GALILEO_CONF;

static __dead void
usage(void)
{
	fprintf(stderr, "usage: %s [-dnv] [-D macro=value] [-f file]",
	    getprogname());
	exit(1);
}

int
main(int argc, char **argv)
{
	struct galileo	*env;
	struct privsep	*ps;
	const char	*errstr;
	const char	*title = NULL;
	size_t		 i;
	int		 conftest = 0, debug = 0, verbose = 0;
	int		 argc0 = argc, ch;
	int		 proc_id = PROC_PARENT;
	int		 proc_instance = 0;

	setlocale(LC_CTYPE, "");

	/* log to stderr until daemonized */
	log_init(1, LOG_DAEMON);
	log_setverbose(verbose);

	while ((ch = getopt(argc, argv, "D:df:I:nP:v")) != -1) {
		switch (ch) {
		case 'D':
			if (cmdline_symset(optarg) < 0)
				log_warnx("could not parse macro definition %s",
				    optarg);
			break;
		case 'd':
			debug = 1;
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'I':
			proc_instance = strtonum(optarg, 0, PROC_MAX_INSTANCES,
			    &errstr);
			if (errstr != NULL)
				fatalx("invalid process instance");
			break;
		case 'n':
			conftest = 1;
			break;
		case 'P':
			title = optarg;
			proc_id = proc_getid(procs, nitems(procs), title);
			if (proc_id == PROC_MAX)
				fatalx("invalid process name");
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	if (argc != 0)
		usage();

	if (geteuid())
		fatalx("need root privileges");

	log_setverbose(verbose);

	env = xcalloc(1, sizeof(*env));
	config_init(env);
	if (parse_config(conffile, env) == -1)
		return (1);

	if (conftest) {
		fprintf(stderr, "configuration OK\n");
		return (0);
	}

	ps = xcalloc(1, sizeof(*ps));
	ps->ps_env = env;
	env->sc_ps = ps;
	if ((ps->ps_pw = getpwnam(GALILEO_USER)) == NULL)
		fatalx("unknown user %s", GALILEO_USER);

	ps->ps_instances[PROC_PROXY] = env->sc_prefork;
	ps->ps_instance = proc_instance;
	if (title != NULL)
		ps->ps_title[proc_id] = title;

	if (*env->sc_chroot == '\0') {
		if (strlcpy(env->sc_chroot, ps->ps_pw->pw_dir,
		    sizeof(env->sc_chroot)) >= sizeof(env->sc_chroot))
			fatalx("chroot path too long!");
	}

	for (i = 0; i < nitems(procs); ++i)
		procs[i].p_chroot = env->sc_chroot;

	/* only the parent returns */
	proc_init(ps, procs, nitems(procs), debug, argc0, argv, proc_id);

	log_procinit("parent");
	if (!debug && daemon(0, 0) == -1)
		fatal("failed to daemonize");

	log_init(debug, LOG_DAEMON);

	log_info("startup");

	/* if (pledge("stdio rpath wpath cpath unix fattr sendfd", NULL) == -1) */
		/* fatal("pledge"); */

	event_init();

	signal(SIGPIPE, SIG_IGN);

	signal_set(&ps->ps_evsigint, SIGINT, parent_sig_handler, ps);
	signal_set(&ps->ps_evsigterm, SIGTERM, parent_sig_handler, ps);
	signal_set(&ps->ps_evsigchld, SIGCHLD, parent_sig_handler, ps);
	signal_set(&ps->ps_evsighup, SIGHUP, parent_sig_handler, ps);

	signal_add(&ps->ps_evsigint, NULL);
	signal_add(&ps->ps_evsigterm, NULL);
	signal_add(&ps->ps_evsigchld, NULL);
	signal_add(&ps->ps_evsighup, NULL);

	proc_connect(ps);

	if (parent_configure(env) == -1)
		fatalx("configuration failed");

	event_dispatch();

	parent_shutdown(env);
	/* NOTREACHED */

	return (0);
}

static int
parent_configure(struct galileo *env)
{
	struct proxy	*proxy;
	int		 id;

	TAILQ_FOREACH(proxy, &env->sc_proxies, pr_entry) {
		if (config_setproxy(env, proxy) == -1)
			fatal("send proxy");
	}

	/* XXX: eventually they will be more than just one */
	if (config_setsock(env) == -1)
		fatal("send socket");

	/* The proxiess need to reload their config. */
	env->sc_reload = env->sc_prefork;

	for (id = 0; id < PROC_MAX; id++) {
		if (id == privsep_process)
			continue;
		proc_compose(env->sc_ps, id, IMSG_CFG_DONE, env, sizeof(env));
	}

	config_purge(env);
	return (0);
}

static void
parent_configure_done(struct galileo *env)
{
	int	 id;

	if (env->sc_reload == 0) {
		log_warnx("configuration already finished");
		return;
	}

	env->sc_reload--;
	if (env->sc_reload == 0) {
		for (id = 0; id < PROC_MAX; ++id) {
			if (id == privsep_process)
				continue;

			proc_compose(env->sc_ps, id, IMSG_CTL_START, NULL, 0);
		}
	}
}

static void
parent_reload(struct galileo *env)
{
	if (env->sc_reload) {
		log_debug("%s: already in progress: %d pending",
		    __func__, env->sc_reload);
	}

	log_debug("%s: config file %s", __func__, conffile);

	config_purge(env);

	if (parse_config(conffile, env) == -1) {
		log_warnx("failed to load config file: %s", conffile);
		return;
	}

	config_setreset(env);
	parent_configure(env);
}

static void
parent_sig_handler(int sig, short ev, void *arg)
{
	struct privsep	*ps = arg;

	/*
	 * Normal signal handler rules don't apply because libevent
	 * decouples for us.
	 */

	switch (sig) {
	case SIGHUP:
		if (privsep_process != PROC_PARENT)
			return;
		log_info("reload requested with SIGHUP");
		parent_reload(ps->ps_env);
		break;
	case SIGCHLD:
		log_warnx("one child died, quitting.");
	case SIGTERM:
	case SIGINT:
		parent_shutdown(ps->ps_env);
		break;
	default:
		fatalx("unexpected signal %d", sig);
	}
}

static int
parent_dispatch_proxy(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct privsep	*ps = p->p_ps;
	struct galileo	*env = ps->ps_env;

	switch (imsg->hdr.type) {
	case IMSG_CFG_DONE:
		parent_configure_done(env);
		break;
	default:
		return (-1);
	}

	return (0);
}

static __dead void
parent_shutdown(struct galileo *env)
{
	config_purge(env);

	proc_kill(env->sc_ps);

	free(env->sc_ps);
	free(env);

	log_info("parent terminating, pid %d", getpid());
	exit(0);
}

int
accept_reserve(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
    int reserve, volatile int *counter)
{
	int ret;
	if (getdtablecount() + reserve +
	    *counter >= getdtablesize()) {
		errno = EMFILE;
		return (-1);
	}

	if ((ret = accept4(sockfd, addr, addrlen, SOCK_NONBLOCK)) > -1) {
		(*counter)++;
		log_debug("%s: inflight incremented, now %d",__func__, *counter);
	}
	return (ret);
}
