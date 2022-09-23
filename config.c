/*
 * Copyright (c) 2022 Omar Polo <op@omarpolo.com>
 * Copyright (c) 2011 - 2015 Reyk Floeter <reyk@openbsd.org>
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/tree.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <sys/stat.h>		/* umask */
#include <sys/un.h>		/* sockaddr_un */

#include <errno.h>
#include <event.h>
#include <limits.h>
#include <pwd.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <imsg.h>

#include "proc.h"
#include "log.h"
#include "xmalloc.h"

#include "galileo.h"

int
config_init(struct galileo *env)
{
	/* Global configuration */
	if (privsep_process == PROC_PARENT)
		env->sc_prefork = PROXY_NUMPROC;

	/* Other configuration. */
	TAILQ_INIT(&env->sc_proxies);

	env->sc_sock_fd = -1;

	return 0;
}

void
config_purge(struct galileo *env)
{
	struct proxy	*p;

	while ((p = TAILQ_FIRST(&env->sc_proxies)) != NULL) {
		TAILQ_REMOVE(&env->sc_proxies, p, pr_entry);
		proxy_purge(p);
	}
}

int
config_setproxy(struct galileo *env, struct proxy *p)
{
	struct privsep		*ps = env->sc_ps;

	if (proc_compose(ps, PROC_PROXY, IMSG_CFG_SRV, p, sizeof(*p)) == -1)
		fatal("proc_compose");
	return 0;
}

int
config_getproxy(struct galileo *env, struct imsg *imsg)
{
	struct proxy	*proxy;

	proxy = xcalloc(1, sizeof(*proxy));
	if (IMSG_DATA_SIZE(imsg) != sizeof(*proxy))
		fatalx("%s: bad imsg size", __func__);

	memcpy(proxy, imsg->data, sizeof(*proxy));

	log_debug("%s: server=%s proxy-to=%s:%d (%s)", __func__,
	    proxy->pr_conf.host, proxy->pr_conf.proxy_addr,
	    proxy->pr_conf.proxy_port, proxy->pr_conf.proxy_name);

	TAILQ_INSERT_TAIL(&env->sc_proxies, proxy, pr_entry);

	return 0;
}

int
config_setsock(struct galileo *env)
{
	struct privsep		*ps = env->sc_ps;
	struct passwd		*pw = ps->ps_pw;
	struct sockaddr_un	 sun;
	const char		*path = GALILEO_SOCK;
	int			 id, fd, old_umask;

	/*
	 * open listening socket.
	 *
	 * XXX: move to server.c as server_privinit like httpd once we
	 * support more than one listening socket.
	 */
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1) {
		log_warn("%s: socket", __func__);
		return (-1);
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, path, sizeof(sun.sun_path));

	if (unlink(path) == -1)
		if (errno != ENOENT) {
			log_warn("%s: unlink %s", __func__, path);
			close(fd);
			return (-1);
		}

	old_umask = umask(S_IXUSR|S_IXGRP|S_IWOTH|S_IROTH|S_IXOTH);
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		log_warn("%s: bind: %s (%d)", __func__, path, geteuid());
		close(fd);
		umask(old_umask);
		return (-1);
	}
	umask(old_umask);

	if (chmod(path, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) == -1) {
		log_warn("%s: chmod", __func__);
		close(fd);
		(void)unlink(path);
		return (-1);
	}

	if (chown(path, pw->pw_uid, pw->pw_gid) == -1) {
		log_warn("%s: chown", __func__);
		close(fd);
		(void)unlink(path);
		return (-1);
	}

	if (listen(fd, 5) == -1) {
		log_warn("%s: listen", __func__);
		close(fd);
		(void)unlink(path);
		return (-1);
	}

	for (id = 0; id < PROC_MAX; ++id) {
		int n, m;

		if (id == privsep_process || id != PROC_PROXY)
			continue;

		n = -1;
		proc_range(ps, id, &n, &m);
		for (n = 0; n < m; ++n) {
			int d;

			if ((d = dup(fd)) == -1) {
				log_warn("%s: dup", __func__);
				close(fd);
				return (-1);
			}

			if (proc_compose_imsg(ps, id, n, IMSG_CFG_SOCK,
			    -1, d, NULL, 0) == -1) {
				log_warn("%s: failed to compose "
				    "IMSG_CFG_SOCK", __func__);
				close(fd);
				return (-1);
			}
			if (proc_flush_imsg(ps, id, n) == -1) {
				log_warn("%s: failed to flush", __func__);
				close(fd);
				return (-1);
			}
		}
	}

	close(fd);
	return (0);
}

int
config_getsock(struct galileo *env, struct imsg *imsg)
{
	/* XXX: make it more like httpd/gotwebd' one */
	return imsg->fd;
}

int
config_setreset(struct galileo *env)
{
	struct privsep	*ps = env->sc_ps;
	int		 id;

	for (id = 0; id < PROC_MAX; ++id)
		proc_compose(ps, id, IMSG_CTL_RESET, NULL, 0);

	return (0);
}

int
config_getreset(struct galileo *env, struct imsg *imsg)
{
	config_purge(env);

	return (0);
}
