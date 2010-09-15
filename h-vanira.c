/*
 * Copyright © 2009-2010 Sebastian Thorarensen <indigo176@blinkenshell.org>,
 *			 Torbjörn Lönnemark <tobbez@ryara.net>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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

#include "version.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define RECONNECTION_DELAY 30
#define READ_TIMEOUT 600
#define QUIT_TIMEOUT 4

#define FLAG_OP 1

void error(int status, int errnum, const char *format, ...);

void read_opers(void);
void install_signals(void);
void handle_signal(int);
void reload(void);
int irc_connect(char *, char *);
void irc_cleanup(void);
void handle_forever(char *);
void read_command(char *);

int strscmp(const char *, const char *);
struct oper *getoper(const char *, size_t);

void irc_command_ping(char *);
void irc_command_join(char *);
void irc_command_privmsg(char *, char *);
void irc_command_mode(char *);
void irc_command_mode_o(char *, char);
void irc_command_kick(char *);
void irc_register(void);
void irc_join(void);
void irc_quit(char *);

struct conf {
	char master[512];
	char nick[128];
	char server[257]; /* max length of a host name is 256 chars */
	char port[8];
	char channel[128];
};

void read_conf(struct conf *);

struct oper {
	char mask[512];
	int flags;
	struct oper *next;
};

struct conf cfg;
struct oper opers;

char *path;
int pending_reload = 0;

char *outbuf;
int sockfd;
FILE *sockstream;

int self_flags = 0;

int main(int argc, char *argv[])
{
	char *buf;

	path = argv[0];

	read_conf(&cfg);
	read_opers();

	buf = malloc(512 * sizeof(char));
	if (!buf)
		error(EXIT_FAILURE, 0, "Cannot allocate memory");

	install_signals();

	if (argc > 1) {
		sockfd = atoi(argv[1]);
		if (sockfd < 1) /* TODO add fail safe check */
			error(EXIT_FAILURE, 0, "Bad reload socket");
		sockstream = fdopen(sockfd, "w");
		if (!sockstream)
			error(0, errno, "fdopen");
		else
			goto skip_connect;
		
	}

	for (;;) {
		while (!irc_connect(cfg.server, cfg.port))
			sleep(RECONNECTION_DELAY);
skip_connect:	handle_forever(buf);
		printf("Disconnected!\n");
	}

	return EXIT_SUCCESS;
}

void error(int status, int errnum, const char *format, ...)
{
	fprintf(stderr, "%s", path);
	if (format) {
		va_list ap;
		va_start(ap, format);
		fprintf(stderr, ": ");
		vfprintf(stderr, format, ap);
		va_end(ap);
	}
	if (errnum != 0)
		fprintf(stderr, ": %s\n", strerror(errnum));
	else
		fprintf(stderr, "\n");

	if (status != 0)
		exit(status);
}

void read_conf(struct conf *cfg)
{
	FILE *cfgf;
	char buf[512]; 
	char *val;
	unsigned int index;
	int lno;

	cfgf = fopen("config", "r");
	if (!cfgf) {
		error(0, errno, "Couldn't read config file");
		if (errno == ENOENT) {
			printf("Creating config...\n");
			cfgf = fopen("config", "w");
			fputs("master user@host\n"
			      "nick botnick\n"
			      "server host.tld\n"
			      "port 6667\n"
			      "channel #channel\n",
			      cfgf);
			if (fclose(cfgf) == EOF)
				error(0, errno, "fclose");
		}
		exit(3);
	}

	lno = 0;
	while (fgets(buf, 512, cfgf)) { /* read lines */
		lno++;

		if (buf[strlen(buf) - 1] == '\n')
			buf[strlen(buf) - 1] = '\0';

		val = 0;
		for (index = 0; index < strlen(buf); index++) { 
			if(buf[index] == ' ') {
				val = (char *)
				      (buf + (index + 1) * sizeof(*buf));
				buf[index] = '\0';
				break;
			}
		}

		if (val == 0)
			error(4, 0, "Malformed config file on line %i", lno);

		if (strcmp(buf, "server") == 0) {
			strcpy(cfg->server, val);
		} else if (strcmp(buf, "port") == 0) {
			if (strlen(val) > 7)
				error(4, 0, "Malformed config file on line %i",
				      lno);
			strcpy(cfg->port, val);
		} else if (strcmp(buf, "nick") == 0) {
			if (strlen(val) > 127)
				error(4, 0, "Malformed config file on line %i",
				      lno);
			strcpy(cfg->nick, val);
		} else if (strcmp(buf, "channel") == 0) {
			if(strlen(val) > 127)
				error(4, 0, "Malformed config file on line %i",
				      lno);
			strcpy(cfg->channel, val);
		} else if (strcmp(buf, "master") == 0) {
			strcpy(cfg->master, val);
		} else {
			error(4, 0, "Malformed config file on line %i", lno);
		}
	}
	if (fclose(cfgf) == EOF)
		error(0, errno, "fclose");
}

void read_opers(void)
{
	FILE *maskf;
	struct oper *current = &opers;
	char c;

	maskf = fopen("opers", "r");
	if (!maskf) {
		error(0, errno, "Cannot read opers file");
		return;
	}

	strcpy(current->mask, cfg.master);
	current->flags = 0;
	current->next = NULL;

	/* peek for EOF so that we don't allocate one struct too much */
	while ((c=getc(maskf)) != EOF) {
		int len;
		
		ungetc(c, maskf);

		current->next = malloc(sizeof(struct oper));
		if (!current->next)
			error(EXIT_FAILURE, 0, "Cannot allocate memory");
		current = current->next;
		current->flags = 0;
		current->next = NULL;

		len = ftell(maskf);
		if (!fgets(current->mask, 512, maskf))
			error(0, errno, "fgets");
		len = ftell(maskf) - len - 1;
		current->mask[len] = '\0'; /* remove \n */
	}

	if (fclose(maskf) == EOF)
		error(0, errno, "fclose");

}

void install_signals(void)
{
	int signals[] = {SIGHUP, SIGINT, SIGSEGV, SIGTERM, SIGUSR1};
	size_t len = sizeof(signals) / sizeof(int);
	struct sigaction action;
	size_t i;

	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;

	for (i = 0; i < len; i++) {
		if (sigaction(signals[i], &action, NULL) < 0)
			error(EXIT_FAILURE, errno, "sigaction");
	}
}

void handle_signal(int sig)
{
	char *quitmsg;
	int exitval = EXIT_SUCCESS;

	switch (sig) {
		case SIGHUP:
			quitmsg = "Terminal hangup";
			break;
		case SIGINT:
			quitmsg = "Keyboard interrupt";
			break;
		case SIGSEGV:
			quitmsg = "INVALID MEMORY REFERENCE";
			exitval = 139;
			break;
		case SIGTERM:
			quitmsg = "Caught termination signal";
			break;
		case SIGUSR1:
			/* 
			 * will reload when all pending IRC commands 
			 * have been parsed
			 */
			pending_reload = 1;
		default:
			return;
	}

	if (sockstream)
		irc_quit(quitmsg);

	exit(exitval);
}

void reload(void)
{
	/*
	 * hack:
	 * the buffer will always fit an integer if 16 bit or larger
	 */
	char arg[sizeof(int)*3];

	sprintf(arg, "%i", sockfd);
	if (execl(path, "h-vanira", arg, (char *)NULL) < 0)
		error(0, errno, "execl");
}

int irc_connect(char *hostname, char *port)
{
	struct addrinfo hints;
	struct addrinfo *ai;
	int errcode;

	hints.ai_flags = 0;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	errcode = getaddrinfo(hostname, port, &hints, &ai);
	if (errcode < 0) {
		error(0, 0, "getaddrinfo: %s", gai_strerror(errcode));
		return 0;
	}

	sockfd = socket(ai->ai_family, SOCK_STREAM, 0);
	if (sockfd < 0) {
		error(0, errno, "socket");
		return 0;
	}

	if (connect(sockfd, ai->ai_addr, ai->ai_addrlen) == -1) {
		error(0, errno, "connect");
		freeaddrinfo(ai);
		return 0;
	}

	sockstream = fdopen(sockfd, "w");
	if (!sockstream) {
		error(0, errno, "fdopen");
		freeaddrinfo(ai);
		return 0;
	}

	irc_register();

	freeaddrinfo(ai);
	return 1;
}

void irc_cleanup(void)
{
	fclose(sockstream);
	close(sockfd);
}

void handle_forever(char *buf)
{
	fd_set sockset;
	struct timeval tv;
	int err;

	size_t offset = 0;
	ssize_t rsize = 0;
	ssize_t _rsize;

	FD_ZERO(&sockset);
	FD_SET(sockfd, &sockset);
	tv.tv_sec = READ_TIMEOUT;
	tv.tv_usec = 0;

	while ((err = select(sockfd+1, &sockset, NULL, NULL, &tv)) != 0) {
		/* on Linux timeout gets modified */
		tv.tv_sec = READ_TIMEOUT;
		tv.tv_usec = 0;

		if (err < 0) {
			if (errno == EINTR) {
				if (pending_reload && offset == 0) {
					reload();
					pending_reload = 0;
				}
				continue;
			} else {
				error(0, errno, "select");
				irc_cleanup();
				return; /* reconnect */
			}
		}

		_rsize = read(sockfd, buf+offset, 512-offset);
		switch (_rsize) {
			case -1:
				error(0, errno, "read");
			case 0:
				irc_cleanup();
				return; /* reconnect */
			default:
				break;
		}
		
		rsize += _rsize;
		while (offset < (size_t)rsize) {
			if (!(buf[offset++] == '\r' && buf[offset] == '\n')) 
				continue; /* search for \r\n */

			buf[offset-1] = '\0';
			read_command(buf);
			offset++;
			if ((size_t)rsize == offset) {
				/*
				 * only got one command,
				 * either goto read
				 * or reload if we have a pending reload
				 */
				if (pending_reload) {
					reload();
					pending_reload = 0;
				}
				rsize = 0;
				offset = 0;
				break;
			}

			/*
			 * more commands on the buffer
			 * move command to beginning of buffer and
			 * continue search for \r\n 
			 */
			rsize -= offset;
			memmove(buf, buf+offset, rsize);
			offset = 0;
			continue;
		}

		/* 
		 * avoid infinite loop if we receive a message
		 * which is too long
		 */
		if (offset == 512) 
			offset = 0;
	}

	/* select timed out */
	error(0, 0, "No server activity for %i seconds", READ_TIMEOUT);
	irc_quit("Nobody ever talks to me T_T");
	irc_cleanup();
}

void read_command(char *msg)
{
	char *cmd = msg;

	/* For debugging: */
	printf("%s\n", msg);

	/* strip prefix from cmd */
	if (cmd[0] == ':') {
		do {
			cmd++;
		} while (cmd[0] != ' ' && cmd[1] != '\0');
		cmd[0] = '\0';
		cmd++;
	}

	/* maybe optimized lookup */
	switch (cmd[0]) {
		int params;

	case '2':
		params = strscmp(cmd, "251");
		if (params > 0) {
			/* registered, we can now join channels */
			irc_join();
		}
		return;

	case 'J':
		params = strscmp(cmd, "JOIN");
		if (params > 0) {
			irc_command_join(msg);
		}
		return;

	case 'P':
		params = strscmp(cmd, "PING");
		if (params > 0) {
			irc_command_ping(cmd+params);
			return;
		}

		params = strscmp(cmd, "PRIVMSG");
		if (params > 0) {
			irc_command_privmsg(msg, cmd+params);
		}
		return;

	case 'K':
		params = strscmp(cmd, "KICK");
		if (params > 0) {
			irc_command_kick(cmd+params);
		}
		return;

	}
}

/*
 * kind of strcmp, but also stops at space in s1
 * returns strlen + 1
 */
int strscmp(const char *s1, const char *s2)
{
	int i;
	for (i = 0; s1[i] != ' ' && s1[i] != '\0' && s2[i] != '\0'; i++) {
		if (s1[i] != s2[i])
			return -1;
	}
	return i + 1;
}

/*
 * get oper by hostmask or NULL if the mask doesn't belong to an oper
 */
struct oper *getoper(const char *mask, size_t n)
{
	struct oper *o;

	for (o=&opers; o; o=o->next) {
		if (memcmp(mask, o->mask, n) == 0)
			return o;
	}

	return NULL;
}

void irc_command_ping(char *params)
{
	fprintf(sockstream, "PONG %s\r\n", params);
	fflush(sockstream);
}

/*
 * ops users in opers list on join
 */
void irc_command_join(char *prefix)
{
	char *mask;
	struct oper *o;
	size_t len;

	/* skip colon */
	prefix++;

	mask = strchr(prefix, '!');
	if (!mask)
		return;
	mask[0] = '\0';
	mask++;

	len = strlen(mask);
	if (len > 512)
		return; /* mask to long for us */

	o = getoper(mask, len);
	if (o == NULL || o->flags & FLAG_OP)
		return;

	fprintf(sockstream, "MODE %s +o %s\r\n", cfg.channel, prefix);
	fflush(sockstream);
	return;
}

void irc_command_privmsg(char *prefix, char *params)
{
	char *msg;
	char *n;

	/* make nick string */
	prefix++;
	n = strchr(prefix, '!');
	if (!n)
		return;
	n[0] = '\0';

	msg = strchr(params, ' ');
	if (!msg)
		return;
	msg[0] = '\0';
	msg++;

	if (strcmp(params, cfg.nick) != 0)
		return;

	if (strcmp(msg, ":\001VERSION\001") == 0) {
		fprintf(sockstream, "NOTICE %s :\001VERSION %s\001\r\n",
				prefix, VERSION);
		fflush(sockstream);
	}
}

void irc_command_mode(char *params)
{
	char *modes;
	char *n;
	char *nicks[3];
	int i;
	int nickc;
	char modifier;

	modes = strchr(params, ' ');
	if (!modes)
		return;
	modes++;

	i = 0;
	while ((n = strchr(modes, ' '))) {
		if (i > 2)
			return; /* more than three arguments is illegal */
		n[0] = '\0';
		nicks[i++] = ++n;
	}
	if (i < 1)
		return; /* no nicks? */

	nickc = i;
	i = 0;

	modifier = modes[0];
	if (modifier != '-' || modifier != '+')
		return; /* no initial modifier? */
	while ((++modes)[0] != '\0' && i < nickc) {
		switch (modes[0]) {
			case '+':
			case '-':
				modifier = modes[0];
				continue;
			case 'o':
				irc_command_mode_o(nicks[i++], modifier);
				continue;
		}
	}
}

void irc_command_mode_o(char *nick, char mod)
{
	/* TODO sync opers */
}

/*
 * rejoins on kick
 */
void irc_command_kick(char *params)
{
	char *end;

	params = strchr(params, ' '); /* jump to nickname */
	if (!params)
		return;
	params++;

	end = strchr(params, ' ');
	if (!end)
		return;
	end[0] = '\0';

	if (strcmp(params, cfg.nick) == 0) {
		irc_join();
	}
}

void irc_register(void)
{
	fprintf(sockstream, "NICK %s\r\n", cfg.nick);
	fprintf(sockstream, "USER H-Vanira localhost localhost "
			":H-Vanira the Bot\r\n");
	fflush(sockstream);
}

void irc_join(void)
{
	fprintf(sockstream, "JOIN %s\r\n", cfg.channel);
	fflush(sockstream);
}

void irc_quit(char *msg)
{
	fd_set sockset;
	struct timeval tv;

	fprintf(sockstream, "QUIT :%s\r\n", msg);
	fflush(sockstream);

	FD_ZERO(&sockset);
	FD_SET(sockfd, &sockset);
	tv.tv_sec = QUIT_TIMEOUT;
	tv.tv_usec = 0;

	/* wait until server has responded to our quit */
	select(sockfd+1, &sockset, NULL, NULL, &tv);
}
