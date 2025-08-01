/*
 * Copyright (c) 2022 Omar Polo <op@omarpolo.com>
 * Copyright (c) 2006 - 2015 Reyk Floeter <reyk@openbsd.org>
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
#include <netdb.h>

#include <asr.h>
#include <ctype.h>
#include <errno.h>
#include <event.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <imsg.h>
#include <tls.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "proc.h"
#include "tmpl.h"

#include "galileo.h"

#define MINIMUM(a, b)	((a) < (b) ? (a) : (b))

#if HAVE_LIBEVENT2
# define G_TOUT(t)	((t).tv_sec)
#else
# define G_TOUT(t)	(t)
#endif

/* provided by OpenBSD' base libevent but not in any header? */
extern void	 bufferevent_read_pressure_cb(struct evbuffer *, size_t,
		    size_t, void *);

void	proxy_init(struct privsep *, struct privsep_proc *, void *);
int	proxy_launch(struct galileo *);
void	proxy_inflight_dec(const char *);
int	proxy_dispatch_parent(int, struct privsep_proc *, struct imsg *);
int	proxy_resurl(struct client *, const char *, char *, size_t);
void	proxy_translate_gemtext(struct client *);
void	proxy_resolved(struct asr_result *, void *);
void	proxy_connect(int, short, void *);
int	proxy_start_reply(struct client *, int, const char *);
void	proxy_read(struct bufferevent *, void *);
void	proxy_write(struct bufferevent *, void *);
void	proxy_error(struct bufferevent *, short, void *);
int	proxy_bufferevent_add(struct event *, int);
void	proxy_tls_writecb(int, short, void *);
void	proxy_tls_readcb(int, short, void *);

static struct privsep_proc procs[] = {
	{ "parent",	PROC_PARENT, proxy_dispatch_parent },
};

volatile int proxy_clients;
volatile int proxy_inflight;
uint32_t proxy_fcg_id;

void
proxy(struct privsep *ps, struct privsep_proc *p)
{
	proc_run(ps, p, procs, nitems(procs), proxy_init, NULL);
}

void
proxy_init(struct privsep *ps, struct privsep_proc *p, void *arg)
{
	if (config_init(ps->ps_env) == -1)
		fatal("failed to initialize configuration");

	/* We use a custom shutdown callback */
	/* p->p_shutdown = proxy_shutdown */

	if (pledge("stdio recvfd unix inet dns", NULL) == -1)
		fatal("pledge");
}

int
proxy_launch(struct galileo *env)
{
	event_add(&env->sc_evsock, NULL);
	return (0);
}

void
proxy_purge(struct proxy *pr)
{
	free(pr);
}

void
proxy_inflight_dec(const char *why)
{
	proxy_inflight--;
	log_debug("%s: inflight decremented, now %d, %s",
	    __func__, proxy_inflight, why);
}

int
proxy_dispatch_parent(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct privsep	*ps = p->p_ps;
	struct galileo	*env = ps->ps_env;

	switch (imsg->hdr.type) {
	case IMSG_CFG_SRV:
		if (config_getproxy(env, imsg) == -1)
			fatal("config_getproxy");
		break;
	case IMSG_CFG_SOCK:
		/* XXX: improve */

		env->sc_sock_fd = config_getsock(env, imsg);
		if (env->sc_sock_fd == -1)
			fatal("config_getsock");

		event_set(&env->sc_evsock, env->sc_sock_fd,
		    EV_READ | EV_PERSIST, fcgi_accept, env);
		evtimer_set(&env->sc_evpause, fcgi_accept, env);
		break;
	case IMSG_CFG_DONE:
		config_getcfg(env, imsg);
		proxy_launch(env);
		break;
	case IMSG_CTL_START:
		break;
	case IMSG_CTL_RESET:
		config_getreset(env, imsg);
		break;
	default:
		return (-1);
	}

	return (0);
}

static int
portok(const char **url, struct client *clt)
{
	const char		*u = *url;
	const char		*port = clt->clt_pc->proxy_port;
	size_t			 len;

	len = strlen(port);

	if (*u == '\0' || *u == '/' || *u == '?' || *u == '#')
		return (1);
	if (*u != ':')
		return (0);
	u++;
	if (strncmp(u, port, len) != 0)
		return (0);
	u += len;
	if (*u != '\0' && *u != '/' && *u != '?' && *u != '#')
		return (0);
	*url = u;
	return (1);
}

int
proxy_resurl(struct client *clt, const char *url, char *buf, size_t len)
{
	const char		*tmp;
	size_t			 l;

	if (clt->clt_server_name == NULL)
		return (-1);

	l = strlen(clt->clt_server_name);

	if (len == 0) {
		log_warn("%s: zero-sized buffer!", __func__);
		return (-1);
	}

	/* look if it's an absolute URI */
	if (strncmp(url, "//", 2) == 0) {
		tmp = url + 2;
		if (strncmp(tmp, clt->clt_server_name, l) != 0)
			goto done;

		tmp += l;
		if (!portok(&tmp, clt))
			goto done;
		url = tmp;
	} else if (strncmp(url, "gemini://", 9) == 0) {
		tmp = url + 9;
		if (strncmp(tmp, clt->clt_server_name, l) != 0)
			goto done;

		tmp += l;
		if (!portok(&tmp, clt))
			goto done;
		url = tmp;
	} else {
		tmp = url;
		while (isalpha((unsigned char)*tmp) || *tmp == '+')
			tmp++;
		if (strncmp(tmp, "://", 3) == 0)
			goto done;
	}

	/* maybe it's an absolute path? */
	if (*url == '\0' || *url == '/') {
		if (strlcpy(buf, clt->clt_script_name, len) >= len)
			return (-1);
		if (strlcat(buf, url + 1, len) >= len)
			return (-1);
		return (0);
	}

	/* otherwise, leave it as it is */
 done:
	if (strlcpy(buf, url, len) >= len)
		return (-1);
	return (0);
}

static inline int
match_image_heur(const char *url)
{
	return (fnmatch("*.jpg", url, 0) == 0 ||
	    fnmatch("*.jpeg", url, 0) == 0 ||
	    fnmatch("*.gif", url, 0) == 0 ||
	    fnmatch("*.png", url, 0) == 0 ||
	    fnmatch("*.svg", url, 0) == 0 ||
	    fnmatch("*.webp", url, 0) == 0);
}

static int
gemtext_translate_line(struct client *clt, char *line)
{
	struct template	*tp = clt->clt_tp;
	char		 buf[1025];
	char		*url;

	/* preformatted line / closing */
	if (clt->clt_translate & TR_PRE) {
		if (!strncmp(line, "```", 3)) {
			clt->clt_translate &= ~TR_PRE;
			return (tp_pre_close(clt->clt_tp));
		}

		if (tp_htmlescape(clt->clt_tp, line) == -1)
			return (-1);
		return (tp_write(tp, "\n", 1));
	}

	/* bullet */
	if (!strncmp(line, "* ", 2)) {
		if (clt->clt_translate & TR_NAV) {
			if (tp_writes(tp, "</ul></nav>") == -1)
				return (-1);
			clt->clt_translate &= ~TR_NAV;
		}

		if (!(clt->clt_translate & TR_LIST)) {
			if (tp_writes(tp, "<ul>") == -1)
				return (-1);
			clt->clt_translate |= TR_LIST;
		}

		if (tp_writes(tp, "<li>") == -1 ||
		    tp_htmlescape(clt->clt_tp, line + 2) == -1 ||
		    tp_writes(tp, "</li>") == -1)
			return (-1);
		return (0);
	}

	if (clt->clt_translate & TR_LIST) {
		if (tp_writes(tp, "</ul>") == -1)
			return (-1);
		clt->clt_translate &= ~TR_LIST;
	}

	/* link */
	if (!strncmp(line, "=>", 2)) {
		char *label;

		line += 2;
		line += strspn(line, " \t");

		label = line + strcspn(line, " \t");
		if (*label == '\0')
			label = line;
		else {
			*label++ = '\0';
			label += strspn(label, " \t");
			if (*label == '\0')
				label = line;
		}

		if (proxy_resurl(clt, line, buf, sizeof(buf)) == 0)
			url = buf;
		else
			url = line; /* leave the URL as it is */

		if (!(clt->clt_pc->flags & PROXY_NO_IMGPRV) &&
		    match_image_heur(url)) {
			if (clt->clt_translate & TR_NAV) {
				if (tp_writes(tp, "</ul></nav>") == -1)
					return (-1);
				clt->clt_translate &= ~TR_NAV;
			}

			if (tp_figure(tp, url, label) == -1)
				return (-1);

			return (0);
		}

		if (!(clt->clt_translate & TR_NAV)) {
			if (tp_writes(tp, "<nav><ul>") == -1)
				return (-1);
			clt->clt_translate |= TR_NAV;
		}

		if (tp_writes(tp, "<li><a href='") == -1)
			return (-1);

		if (tp_urlescape(clt->clt_tp, url) == -1 ||
		    tp_writes(tp, "'>") == -1 ||
		    tp_htmlescape(clt->clt_tp, label) == -1 ||
		    tp_writes(tp, "</a></li>") == -1)
			return (-1);

		return (0);
	}

	if (clt->clt_translate & TR_NAV) {
		if (tp_writes(tp, "</ul></nav>") == -1)
			return (-1);
		clt->clt_translate &= ~TR_NAV;
	}

	/* pre opening */
	if (!strncmp(line, "```", 3)) {
		line += 3;
		line += strspn(line, " \t");

		clt->clt_translate |= TR_PRE;
		return (tp_pre_open(tp, line));
	}

	/* citation block */
	if (*line == '>') {
		if (tp_writes(tp, "<blockquote>") == -1 ||
		    tp_htmlescape(clt->clt_tp, line + 1) == -1 ||
		    tp_writes(tp, "</blockquote>") == -1)
			return (-1);
		return (0);
	}

	/* headings */
	if (!strncmp(line, "###", 3)) {
		if (tp_writes(tp, "<h3>") == -1 ||
		    tp_htmlescape(clt->clt_tp, line + 3) == -1 ||
		    tp_writes(tp, "</h3>") == -1)
			return (-1);
		return (0);
	}
	if (!strncmp(line, "##", 2)) {
		if (tp_writes(tp, "<h2>") == -1 ||
		    tp_htmlescape(clt->clt_tp, line + 2) == -1 ||
		    tp_writes(tp, "</h2>") == -1)
			return (-1);
		return (0);
	}
	if (!strncmp(line, "#", 1)) {
		if (tp_writes(tp, "<h1>") == -1 ||
		    tp_htmlescape(clt->clt_tp, line + 1) == -1 ||
		    tp_writes(tp, "</h1>") == -1)
			return (-1);
		return (0);
	}

	/* Not following strictly the gemini specification... */
	if (*line == '\0')
		return (0);

	/* paragraph */
	if (tp_writes(tp, "<p>") == -1 ||
	    tp_htmlescape(clt->clt_tp, line) == -1 ||
	    tp_writes(tp, "</p>") == -1)
		return (-1);

	return (0);
}

void
proxy_translate_gemtext(struct client *clt)
{
	struct bufferevent	*bev = clt->clt_bev;
	struct evbuffer		*src = EVBUFFER_INPUT(bev);
	char			*line;
	size_t			 len;
	int			 r;

	for (;;) {
		line = evbuffer_readln(src, &len, EVBUFFER_EOL_CRLF);
		if (line == NULL)
			return;

		r = gemtext_translate_line(clt, line);
		free(line);
		if (r == -1)
			return;
	}
}

struct proxy_config *
proxy_match(struct galileo *env, const char *name)
{
	struct proxy		*pr;

	if (name == NULL)
		return (NULL);

	TAILQ_FOREACH(pr, &env->sc_proxies, pr_entry) {
		if (!strcmp(name, pr->pr_conf.host))
			return (&pr->pr_conf);
	}

	return (NULL);
}

int
proxy_start_request(struct galileo *env, struct client *clt)
{
	struct addrinfo		 hints;
	struct asr_query	*query;
	int			 r;
	char			*url;

	if (clt->clt_path_info == NULL) {
		log_warnx("PATH_INFO not defined!");
		if (proxy_start_reply(clt, 501, "text/html") == -1)
			return (-1);
		if (tp_error(clt->clt_tp, -1, "internal server error") == -1)
			return (-1);
		return (fcgi_end_request(clt, 1));
	}

	if ((clt->clt_pc = proxy_match(env, clt->clt_server_name)) == NULL) {
		if (proxy_start_reply(clt, 501, "text/html") == -1)
			return (-1);
		if (tp_error(clt->clt_tp, -1, "unknown server") == -1)
			return (-1);
		return (fcgi_end_request(clt, 1));
	}

	if (clt->clt_bodylen != 0 && clt->clt_body == NULL) {
		if (proxy_start_reply(clt, 400, "text/html") == -1)
			return (-1);
		if (tp_error(clt->clt_tp, -1, "bad request") == -1)
			return (-1);
		return (fcgi_end_request(clt, 1));
	}

	if (clt->clt_body) {
		r = asprintf(&url, "%s%s?%s", clt->clt_script_name,
		    clt->clt_path_info + 1, clt->clt_body);
		if (r == -1)
			return (fcgi_end_request(clt, 1));

		if (proxy_start_reply(clt, 302, url) == -1 ||
		    fcgi_end_request(clt, 1) == -1) {
			free(url);
			return (-1);
		}
		free(url);
		return (0);
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	query = getaddrinfo_async(clt->clt_pc->proxy_addr,
	    clt->clt_pc->proxy_port, &hints, NULL);
	if (query == NULL) {
		log_warn("getaddrinfo_async");
		return (fcgi_abort_request(clt));
	}

	clt->clt_evasr = event_asr_run(query, proxy_resolved, clt);
	if (clt->clt_evasr == NULL) {
		log_warn("event_asr_run");
		asr_abort(query);
		return (fcgi_abort_request(clt));
	}

	return (0);
}

void
proxy_resolved(struct asr_result *res, void *d)
{
	struct client		*clt = d;
	struct proxy_config	*pc = clt->clt_pc;

	clt->clt_evasr = NULL;

	if (res->ar_gai_errno != 0) {
		log_warnx("failed to resolve %s:%s: %s",
		    pc->proxy_addr, pc->proxy_port,
		    gai_strerror(res->ar_gai_errno));
		if (proxy_start_reply(clt, 501, "text/html") == -1)
			return;
		if (tp_error(clt->clt_tp, -1, "Can't resolve host") == -1)
			return;
		fcgi_end_request(clt, 1);
		return;
	}

	clt->clt_addrinfo = res->ar_addrinfo;
	clt->clt_p = clt->clt_addrinfo;
	proxy_connect(-1, 0, clt);
}

void
proxy_connect(int fd, short ev, void *d)
{
	struct client		*clt = d;
	struct evbuffer		*out;
	struct addrinfo		*p;
	struct tls_config	*conf;
	struct timeval		 conntv = {5, 0};
	int			 err = 0;
	socklen_t		 len = sizeof(err);

again:
	if (clt->clt_p == NULL)
		goto err;

	if (clt->clt_fd != -1) {
		if (getsockopt(clt->clt_fd, SOL_SOCKET, SO_ERROR, &err, &len)
		    == -1)
			goto err;
		if (err != 0) {
			errno = err;
			goto err;
		}
		goto done;
	}

	p = clt->clt_p;
	clt->clt_fd = socket(p->ai_family, p->ai_socktype | SOCK_NONBLOCK,
	    p->ai_protocol);
	if (clt->clt_fd == -1) {
		clt->clt_p = clt->clt_p->ai_next;
		goto again;
	}

	if (connect(clt->clt_fd, p->ai_addr, p->ai_addrlen) == 0)
		goto done;

	clt->clt_evconn_live = 1;
	event_set(&clt->clt_evconn, clt->clt_fd, EV_WRITE, proxy_connect, clt);
	event_add(&clt->clt_evconn, &conntv);
	return;

done:
	clt->clt_evconn_live = 0;
	freeaddrinfo(clt->clt_addrinfo);
	clt->clt_addrinfo = clt->clt_p = NULL;

	clt->clt_bev = bufferevent_new(clt->clt_fd, proxy_read, proxy_write,
	    proxy_error, clt);
	if (clt->clt_bev == NULL) {
		log_warn("bufferevent_new");
		goto err;
	}

	if (!(clt->clt_pc->flags & PROXY_NO_TLS)) {
		/* initialize TLS for Gemini */
		if ((conf = tls_config_new()) == NULL) {
			log_warn("tls_config_new failed");
			goto err;
		}

		tls_config_insecure_noverifycert(conf);

		if ((clt->clt_ctx = tls_client()) == NULL) {
			log_warnx("tls_client failed");
			tls_config_free(conf);
			goto err;
		}

		if (tls_configure(clt->clt_ctx, conf) == -1) {
			log_warnx("tls_configure failed");
			tls_config_free(conf);
			goto err;
		}

		tls_config_free(conf);

		if (tls_connect_socket(clt->clt_ctx, clt->clt_fd,
			clt->clt_pc->proxy_name) == -1) {
			log_warnx("tls_connect_socket failed");
			goto err;
		}

		event_set(&clt->clt_bev->ev_read, clt->clt_fd, EV_READ,
		    proxy_tls_readcb, clt->clt_bev);
		event_set(&clt->clt_bev->ev_write, clt->clt_fd, EV_WRITE,
		    proxy_tls_writecb, clt->clt_bev);

#if HAVE_LIBEVENT2
		evbuffer_unfreeze(clt->clt_bev->input, 0);
		evbuffer_unfreeze(clt->clt_bev->output, 1);
#endif
	}

	/* bufferevent_settimeout(); */
	bufferevent_enable(clt->clt_bev, EV_READ|EV_WRITE);

	/* TODO: compute the URL */
	out = EVBUFFER_OUTPUT(clt->clt_bev);
	if (evbuffer_add_printf(out, "gemini://%s/%s",
	    clt->clt_pc->proxy_name, clt->clt_path_info) == -1) {
		log_warn("bufferevent_printf failed");
		goto err;
	}

	if (clt->clt_query &&
	    evbuffer_add_printf(out, "?%s", clt->clt_query) == -1) {
		log_warn("bufferevent_printf failed");
		goto err;
	}

	if (evbuffer_add(out, "\r\n", 2) == -1) {
		log_warn("bufferevent_add failed");
		goto err;
	}

	return;

err:
	log_warn("failed to connect to %s:%s",
	    clt->clt_pc->proxy_addr, clt->clt_pc->proxy_port);
	if (proxy_start_reply(clt, 501, "text/html") == -1)
		return;
	if (tp_error(clt->clt_tp, -1, "Can't connect") == -1)
		return;
	fcgi_end_request(clt, 1);
}

static inline int
parse_mime(struct client *clt, char *mime, char *lang, size_t len)
{
	char			*t, *semi;

	memset(lang, 0, len);

	if (strncmp(mime, "text/gemini", 11) != 0)
		return (0);

	clt->clt_translate = TR_ENABLED;

	if ((mime = strchr(mime, ';')) == NULL)
		return (0);

	*mime++ = '\0';
	while ((t = strsep(&mime, ";")) != NULL) {
		if (!strncmp(t, "charset=", 8)) {
			t += 8;
			if (!strncasecmp(t, "utf8", 4) ||
			    !strncasecmp(t, "utf-8", 5) ||
			    !strncasecmp(t, "ascii", 5)) {
				log_debug("unknown charset %s", t);
				return (-1);
			}
			continue;
		}

		if (!strncmp(t, "lang=", 5)) {
			t += 5;
			if ((semi = strchr(t, ';')) != NULL)
				*semi = '\0';

			if (strlcpy(lang, t, len) >= len) {
				log_debug("lang too long: %s", t);
				*lang = '\0';
			}

			if (semi)
				*semi = ';';
			continue;
		}
	}

	return (0);
}

int
proxy_start_reply(struct client *clt, int status, const char *ctype)
{
	struct template	*tp = clt->clt_tp;
	const char	*csp;

	csp = "Content-Security-Policy: default-src 'self'; "
	    "script-src 'none'; object-src 'none';\r\n";

	if (status != 200 &&
	    tp_writef(tp, "Status: %d\r\n", status) == -1)
		return (-1);

	if (tp_writes(tp, csp) == -1)
		return (-1);

	if (status == 302) {
		/* use "ctype" as redirect target */
		if (tp_writef(tp, "Location: %s\r\n", ctype) == -1)
			return (-1);
		if (tp_writes(tp, "\r\n") == -1)
			return (-1);
		return (0);
	}

	if (ctype != NULL) {
		if (!strcmp(ctype, "text/html"))
			ctype = "text/html;charset=utf-8";
		if (tp_writef(tp, "Content-Type: %s\r\n", ctype)
		    == -1)
			return (-1);
	}

	if (tp_writes(tp, "\r\n") == -1)
		return (-1);

	return (0);
}

void
proxy_read(struct bufferevent *bev, void *d)
{
	struct client		*clt = d;
	struct evbuffer		*src = EVBUFFER_INPUT(bev);
	const char		*ctype;
	char			 buf[1025];
	char			 lang[16];
	char			*hdr, *mime;
	size_t			 len;
	int			 code;

	if (clt->clt_headersdone) {
		if (clt->clt_translate)
			proxy_translate_gemtext(clt);
		else
			clt_write_bufferevent(clt, bev);
		return;
	}

	hdr = evbuffer_readln(src, &len, EVBUFFER_EOL_CRLF_STRICT);
	if (hdr == NULL) {
		if (EVBUFFER_LENGTH(src) >= 1026)
			proxy_error(bev, EV_READ, clt);
		return;
	}

	if (len < 4 ||
	    !isdigit((unsigned char)hdr[0]) ||
	    !isdigit((unsigned char)hdr[1]) ||
	    hdr[2] != ' ') {
		log_warnx("invalid ");
		proxy_error(bev, EV_READ, clt);
		goto err;
	}

	code = (hdr[0] - '0') * 10 + (hdr[1] - '0');

	switch (hdr[0]) {
	case '1':
		if (proxy_start_reply(clt, 200, "text/html") == -1)
			goto err;
		if (tp_inputpage(clt->clt_tp, &hdr[3]) == -1)
			goto err;
		fcgi_end_request(clt, 0);
		goto err;
	case '2':
		/* handled below */
		break;
	case '3':
		if (proxy_resurl(clt, &hdr[3], buf, sizeof(buf)) == 0) {
			if (proxy_start_reply(clt, 302, buf) == -1)
				goto err;
			fcgi_end_request(clt, 0);
			goto err;
		}
		/* fallthrough */
	default:
		if (proxy_start_reply(clt, 501, "text/html") == -1)
			goto err;
		if (tp_error(clt->clt_tp, code, &hdr[3]) == -1)
			goto err;
		fcgi_end_request(clt, 1);
		goto err;
	}

	mime = hdr + 2 + strspn(hdr + 2, " \t");
	if (parse_mime(clt, mime, lang, sizeof(lang)) == -1) {
		if (proxy_start_reply(clt, 501, "text/html") == -1)
			goto err;
		if (tp_error(clt->clt_tp, -1, "Bad response") == -1)
			goto err;
		fcgi_end_request(clt, 1);
		goto err;
	}

	if (clt->clt_translate)
		ctype = "text/html;charset=utf-8";
	else
		ctype = mime;

	if (tp_writef(clt->clt_tp, "Content-Type: %s\r\n\r\n", ctype) == -1)
		goto err;

	clt->clt_headersdone = 1;

	if (clt->clt_translate &&
	    tp_head(clt->clt_tp, lang, NULL) == -1)
		goto err;

	if (!clt->clt_translate &&
	    template_flush(clt->clt_tp) == -1)
		goto err;

	/*
	 * Trigger the read again so we proceed with the response
	 * body, if any.
	 */
	free(hdr);
	proxy_read(bev, d);
	return;

err:
	free(hdr);
}

void
proxy_write(struct bufferevent *bev, void *d)
{
	return;
}

void
proxy_error(struct bufferevent *bev, short err, void *d)
{
	struct client		*clt = d;
	struct template		*tp = clt->clt_tp;
	int			 status = !(err & EVBUFFER_EOF);

	log_debug("proxy error, shutting down the connection (err: %x)",
	    err);

	if (!clt->clt_headersdone) {
		if (proxy_start_reply(clt, 501, "text/html") == -1)
			return;
		if (tp_error(clt->clt_tp, -1, "Proxy error") == -1)
			return;
	} else if (status == 0) {
		if (clt->clt_translate & TR_PRE) {
			if (tp_pre_close(clt->clt_tp))
				return;
			clt->clt_translate &= ~TR_PRE;
		}

		if (clt->clt_translate & TR_LIST) {
			if (tp_writes(tp, "</ul>") == -1)
				return;
			clt->clt_translate &= ~TR_LIST;
		}

		if (clt->clt_translate & TR_NAV) {
			if (tp_writes(tp, "</ul></nav>") == -1)
				return;
			clt->clt_translate &= ~TR_NAV;
		}

		if (clt->clt_translate &&
		    tp_foot(clt->clt_tp) == -1)
			return;
	}

	fcgi_end_request(clt, status);
}

void
proxy_tls_readcb(int fd, short event, void *arg)
{
	struct bufferevent	*bufev = arg;
	struct client		*clt = bufev->cbarg;
	char			 rbuf[IBUF_READ_SIZE];
	int			 what = EVBUFFER_READ;
	int			 howmuch = IBUF_READ_SIZE;
	ssize_t			 ret;
	size_t			 len;

	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto err;
	}

	if (bufev->wm_read.high != 0)
		howmuch = MINIMUM(sizeof(rbuf), bufev->wm_read.high);

	ret = tls_read(clt->clt_ctx, rbuf, howmuch);
	if (ret == TLS_WANT_POLLIN || ret == TLS_WANT_POLLOUT) {
		goto retry;
	} else if (ret == -1) {
		what |= EVBUFFER_ERROR;
		goto err;
	}
	len = ret;

	if (len == 0) {
		what |= EVBUFFER_EOF;
		goto err;
	}

	if (evbuffer_add(bufev->input, rbuf, len) == -1) {
		what |= EVBUFFER_ERROR;
		goto err;
	}

	proxy_bufferevent_add(&bufev->ev_read, G_TOUT(bufev->timeout_read));

	len = EVBUFFER_LENGTH(bufev->input);
	if (bufev->wm_read.low != 0 && len < bufev->wm_read.low)
		return;
	if (bufev->wm_read.high != 0 && len > bufev->wm_read.high) {
		struct evbuffer *buf = bufev->input;
		event_del(&bufev->ev_read);
		evbuffer_setcb(buf, bufferevent_read_pressure_cb, bufev);
		return;
	}

	if (bufev->readcb != NULL)
		(*bufev->readcb)(bufev, bufev->cbarg);
	return;

retry:
	proxy_bufferevent_add(&bufev->ev_read, G_TOUT(bufev->timeout_read));
	return;

err:
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

int
proxy_bufferevent_add(struct event *ev, int timeout)
{
	struct timeval tv, *ptv = NULL;

	if (timeout) {
		timerclear(&tv);
		tv.tv_sec = timeout;
		ptv = &tv;
	}

	return (event_add(ev, ptv));
}

void
proxy_tls_writecb(int fd, short event, void *arg)
{
	struct bufferevent	*bufev = arg;
	struct client		*clt = bufev->cbarg;
	ssize_t			 ret;
	short			 what = EVBUFFER_WRITE;
	size_t			 len;

	if (event == EV_TIMEOUT) {
		what |= EVBUFFER_TIMEOUT;
		goto err;
	}

	if (EVBUFFER_LENGTH(bufev->output)) {
		ret = tls_write(clt->clt_ctx,
		    EVBUFFER_DATA(bufev->output),
		    EVBUFFER_LENGTH(bufev->output));
		if (ret == TLS_WANT_POLLIN || ret == TLS_WANT_POLLOUT) {
			goto retry;
		} else if (ret == -1) {
			what |= EVBUFFER_ERROR;
			goto err;
		}
		len = ret;
		evbuffer_drain(bufev->output, len);
	}

	if (EVBUFFER_LENGTH(bufev->output) != 0)
		proxy_bufferevent_add(&bufev->ev_write,
		    G_TOUT(bufev->timeout_write));

	if (bufev->writecb != NULL &&
	    EVBUFFER_LENGTH(bufev->output) <= bufev->wm_write.low)
		(*bufev->writecb)(bufev, bufev->cbarg);
	return;

retry:
	proxy_bufferevent_add(&bufev->ev_write, G_TOUT(bufev->timeout_write));
	return;

err:
	(*bufev->errorcb)(bufev, what, bufev->cbarg);
}

void
proxy_client_free(struct client *clt)
{
	if (clt->clt_evasr)
		event_asr_abort(clt->clt_evasr);

	if (clt->clt_addrinfo)
		freeaddrinfo(clt->clt_addrinfo);

	if (clt->clt_evconn_live)
		event_del(&clt->clt_evconn);

	if (clt->clt_fd != -1)
		close(clt->clt_fd);

	if (clt->clt_ctx)
		tls_free(clt->clt_ctx);

	if (clt->clt_bev)
		bufferevent_free(clt->clt_bev);

	template_free(clt->clt_tp);

	free(clt->clt_body);
	free(clt->clt_server_name);
	free(clt->clt_script_name);
	free(clt->clt_path_info);
	free(clt->clt_query);
	free(clt);
}
