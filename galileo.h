/*
 * Copyright (c) 2022 Omar Polo <op@omarpolo.com>
 * Copyright (c) 2006 - 2015 Reyk Floeter <reyk@openbsd.org>
 * Copyright (c) 2006, 2007 Pierre-Yves Ritschard <pyr@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
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

#ifndef nitems
#define nitems(_a) (sizeof((_a)) / sizeof((_a)[0]))
#endif

#define GALILEO_USER		"www"
#define GALILEO_SOCK		"/var/www/run/galileo.sock"
#define CONF_FILE		"/etc/galileo.conf"
#define FD_RESERVE		5
#define PROC_MAX_INSTANCES	32
#define PROXY_NUMPROC		3
#define PROC_PARENT_SOCK_FILENO	3

enum {
	IMSG_NONE,
	IMSG_CFG_START,
	IMSG_CFG_SRV,
	IMSG_CFG_SOCK,
	IMSG_CFG_DONE,
	IMSG_CTL_START,
	IMSG_CTL_RESET,
	IMSG_CTL_RESTART,
	IMSG_CTL_PROCFD,
};

struct galileo;
struct proxy_config;

struct imsg;
struct privsep;
struct privsep_proc;
struct template;
struct tls;

struct client {
	uint32_t		 clt_id;
	int			 clt_fd;
	struct fcgi		*clt_fcgi;
	char			*clt_server_name;
	char			*clt_script_name;
	char			*clt_path_info;
	struct proxy_config	*clt_pc;
	struct event_asr	*clt_evasr;
	struct addrinfo		*clt_addrinfo;
	struct addrinfo		*clt_p;
	struct event		 clt_evconn;
	int			 clt_evconn_live;
	struct tls		*clt_ctx;
	struct bufferevent	*clt_bev;
	int			 clt_headersdone;
	struct template		*clt_tp;

#define TR_ENABLED	0x1
#define TR_PRE		0x2
#define TR_LIST		0x4
#define TR_NAV		0x8
	int			 clt_translate;

	char			 clt_buf[1024];
	size_t			 clt_buflen;

	SPLAY_ENTRY(client)	 clt_nodes;
};
SPLAY_HEAD(client_tree, client);

struct fcgi {
	uint32_t		 fcg_id;
	int			 fcg_s;
	struct client_tree	 fcg_clients;
	struct bufferevent	*fcg_bev;
	int			 fcg_toread;
	int			 fcg_want;
	int			 fcg_padding;
	int			 fcg_type;
	uint16_t		 fcg_rec_id;
	int			 fcg_keep_conn;
	int			 fcg_done;

	struct galileo		*fcg_env;

	SPLAY_ENTRY(fcgi)	 fcg_nodes;
};
SPLAY_HEAD(fcgi_tree, fcgi);

struct proxy_config {
	char		 host[HOST_NAME_MAX + 1];
	char		 stylesheet[PATH_MAX];
	char		 proxy_addr[HOST_NAME_MAX + 1];
	char		 proxy_name[HOST_NAME_MAX + 1];
	uint16_t	 proxy_port; /* TODO: turn into string */
};

struct server {
	TAILQ_ENTRY(server)	 srv_entry;
	struct proxy_config	 srv_conf;
};
TAILQ_HEAD(serverlist, server);

struct galileo {
	char			 sc_conffile[PATH_MAX];
	uint16_t		 sc_prefork;
	char			 sc_chroot[PATH_MAX];
	struct serverlist	 sc_servers;
	struct fcgi_tree	 sc_fcgi_socks;

	struct privsep		*sc_ps;
	int			 sc_reload;

	/* XXX: generalize */
	int		sc_sock_fd;
	struct event	sc_evsock;
	struct event	sc_evpause;
};

extern int privsep_process;

/* config.c */
int	 config_init(struct galileo *);
void	 config_purge(struct galileo *);
int	 config_setserver(struct galileo *, struct server *);
int	 config_getserver(struct galileo *, struct imsg *);
int	 config_setsock(struct galileo *);
int	 config_getsock(struct galileo *, struct imsg *);
int	 config_setreset(struct galileo *);
int	 config_getreset(struct galileo *, struct imsg *);

/* fcgi.c */
int	 fcgi_end_request(struct client *, int);
int	 fcgi_abort_request(struct client *);
void	 fcgi_accept(int, short, void *);
void	 fcgi_read(struct bufferevent *, void *);
void	 fcgi_write(struct bufferevent *, void *);
void	 fcgi_error(struct bufferevent *, short error, void *);
int	 clt_putc(struct client *, char);
int	 clt_puts(struct client *, const char *);
int	 clt_write_bufferevent(struct client *, struct bufferevent *);
int	 clt_flush(struct client *);
int	 clt_write(struct client *, const uint8_t *, size_t);
int	 clt_printf(struct client *, const char *, ...)
	     __attribute__((__format__(printf, 2, 3)))
	     __attribute__((__nonnull__(2)));
int	 clt_tp_puts(struct template *, const char *);
int	 clt_tp_putc(struct template *, int);
int	 fcgi_cmp(struct fcgi *, struct fcgi *);
int	 fcgi_client_cmp(struct client *, struct client *);

/* fragments.tmpl */
int	 tp_head(struct template *, const char *, const char *);
int	 tp_foot(struct template *);
int	 tp_figure(struct template *, const char *, const char *);
int	 tp_error(struct template *, int, const char *);

/* galileo.c */
int	 accept_reserve(int, struct sockaddr *, socklen_t *, int,
	     volatile int *);
/* parse.y */
int	 parse_config(const char *, struct galileo *);
int	 cmdline_symset(char *);

/* proxy.c */
extern volatile int proxy_inflight;
extern uint32_t proxy_fcg_id;

void	 proxy(struct privsep *, struct privsep_proc *);
void	 proxy_purge(struct server *);
void	 proxy_start_request(struct galileo *, struct client *);
void	 proxy_client_free(struct client *);

SPLAY_PROTOTYPE(fcgi_tree, fcgi, fcg_nodes, fcgi_cmp);
SPLAY_PROTOTYPE(client_tree, client, clt_nodes, fcgi_client_cmp);
