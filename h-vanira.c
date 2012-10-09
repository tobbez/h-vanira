/*
 * Copyright © 2009-2012 Sebastian Thorarensen <sebth@lysator.liu.se>,
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

#define _POSIX_C_SOURCE 200112L

#include "version.h"
#include "ucfg.h"

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
#include <pwd.h>

#define RECONNECTION_DELAY 30
#define READ_TIMEOUT 600
#define QUIT_TIMEOUT 4

void error(int status, int errnum, const char *format, ...);

void install_signals(void);
void handle_signal(int);
void reload(void);
int irc_connect(char *, char *);
int bindsock(void);
void irc_cleanup(void);
void handle_forever(char *);
void read_command(char *);

void irc_command_ping(char **);
void irc_command_join(char *);
void irc_command_privmsg(char *, char **);
void irc_command_kick(char **);
void irc_register(void);
void irc_join(void);
void irc_quit(char *);

struct ucfg_node *conf;

char *path;
int pending_reload = 0;

char *outbuf;
int sockfd;
FILE *sockstream;

int self_flags = 0;

int main(int argc, char *argv[])
{
	char *buf;
	int err;

	path = argv[0];

	if ((err = ucfg_read_file(&conf, "config")) != UCFG_OK) {
		printf("%s\n", ucfg_strerror(err));
		return 1;
	}
	
	{
		struct ucfg_node *tmp;
		const char* reqvals[] = {
			"core:master",
			"core:nick",
			"core:server",
			"core:port",
			"core:channel"
		};
		int i;
		for (i = 0; i < 5; ++i) {
			if ((err = ucfg_lookup(&tmp, conf, reqvals[i])) ==
					UCFG_ERR_NODE_INEXISTENT) {
				fprintf(stderr, "error: '%s' must be defined "
						"in config\n", reqvals[i]);
				return 1;
			}
		}
	}

	buf = malloc(512);
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
			handle_forever(buf); /* resume handling */
		
	}

	for (;;) {
		while (!irc_connect(ucfg_lookup_string(conf, "core:server"),
					ucfg_lookup_string(conf, "core:port")))
			sleep(RECONNECTION_DELAY);
		handle_forever(buf);
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

void install_signals(void)
{
	int signals[] = {SIGHUP, SIGINT, SIGSEGV, SIGTERM, SIGUSR1};
	size_t len = sizeof(signals) / sizeof(int);
	struct sigaction action;
	size_t i;

	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;

	for (i = 0; i < len; i++)
		if (sigaction(signals[i], &action, NULL) < 0)
			error(EXIT_FAILURE, errno, "sigaction");
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
	struct addrinfo hints = {};
	struct addrinfo *ai;
	struct addrinfo *aip;
	int errcode;

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	errcode = getaddrinfo(hostname, port, &hints, &ai);
	if (errcode != 0) {
		error(0, 0, "getaddrinfo: %s", gai_strerror(errcode));
		return 0;
	}

	for (aip = ai; aip; aip = aip->ai_next) {
		sockfd = socket(ai->ai_family, ai->ai_socktype,
				ai->ai_protocol);
		if (sockfd == -1) {
			/* Failure; retry. */
			error(0, errno, "socket");
			continue;
		}

		if (!bindsock()) {
			/* The bind failed, cancel. */
			close(sockfd);
			freeaddrinfo(ai);
			return 0;
		}

		if (connect(sockfd, ai->ai_addr, ai->ai_addrlen) != -1)
			break; /* Success. */

		/* Failure; retry. */
		error(0, errno, "connect");
		close(sockfd);
	}
	freeaddrinfo(ai);
	if (!aip)
		/*
		 * This means that we hit the end of the list,
		 * no connection succeeded.
		 */
		 return 0;

	sockstream = fdopen(sockfd, "w");
	if (!sockstream) {
		error(0, errno, "fdopen");
		close(sockfd);
		return 0;
	}

	irc_register();
	return 1;
}

int bindsock(void)
{
	struct ucfg_node *node;
	struct addrinfo *ai;
	struct addrinfo *aip;
	int errcode;

	/* TODO: Move this call to ucfg_lookup out of the connect loop. */
	if (ucfg_lookup(&node, conf, "core:bind") != UCFG_OK)
		return 1;

	errcode = getaddrinfo(node->value, NULL, NULL, &ai);
	if (errcode != 0) {
		error(0, 0, "getaddrinfo: %s", gai_strerror(errcode));
		return 0;
	}

	for (aip = ai; aip; aip = aip->ai_next) {
		if (bind(sockfd, ai->ai_addr, ai->ai_addrlen) != -1)
			break; /* Success. */
		/* Failure; retry. */
		error(0, errno, "bind");
	}
	freeaddrinfo(ai);
	
	return aip != NULL; /* aip will be NULL if no bind succeeded.*/
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
		while (offset < (size_t)rsize - 1) {
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
		if (offset == 512) {
			rsize = 0;
			offset = 0;
		}
	}

	/* select timed out */
	error(0, 0, "No server activity for %i seconds", READ_TIMEOUT);
	irc_quit("Server inactive");
	irc_cleanup();
	printf("Disconnected!\n");
}

void read_command(char *msg)
{
	char *cmd;
	char *params[15];
	char *saveptr;
	unsigned i;

	if (!(cmd = strtok_r(msg, " ", &saveptr)))
		return;
	/* Strip prefix. */
	if (cmd[0] == ':')
		cmd = strtok_r(NULL, " ", &saveptr);
	for (i = 0; i < sizeof(params) / sizeof(*params); i++)
		params[i] = strtok_r(NULL, " ", &saveptr);

	if (strcmp(cmd, "251") == 0)
		/* 251 means registered, after this we can join channels */
		irc_join();
	else if (strcmp(cmd, "JOIN") == 0)
		irc_command_join(msg);
	else if (strcmp(cmd, "PING") == 0)
		irc_command_ping(params);
	else if (strcmp(cmd, "PRIVMSG") == 0)
		irc_command_privmsg(msg, params);
	else if (strcmp(cmd, "KICK") == 0)
		irc_command_kick(params);
}

void irc_command_ping(char *params[])
{
	if (!params[0])
		return;
	fprintf(sockstream, "PONG %s\r\n", params[0]);
	fflush(sockstream);
}

/*
 * ops users in opers list on join
 */
void irc_command_join(char *prefix)
{
	char *mask;
	struct ucfg_node *ops;

	/* skip colon */
	prefix++;

	mask = strchr(prefix, '!');
	if (!mask)
		return;
	mask[0] = '\0';
	mask++;

	if (ucfg_lookup(&ops, conf, "plugins:op:") == UCFG_ERR_NODE_INEXISTENT)
		return;

	do {
		if (strcmp(ops->value, mask) == 0) {
			fprintf(sockstream,
				"MODE %s +o %s\r\n",
				ucfg_lookup_string(conf, "core:channel"),
				prefix);
			fflush(sockstream);
			break;
		}
        } while ((ops = ops->next) != NULL);
}

void irc_command_privmsg(char *prefix, char *params[])
{
	char *n;

	if (!(params[0] && params[1]))
		return;

	/* make nick string */
	prefix++;
	n = strchr(prefix, '!');
	if (!n)
		return;
	n[0] = '\0';

	if (strcmp(params[0], ucfg_lookup_string(conf, "core:nick")) != 0)
		return;

	if (strcmp(params[1], ":\001VERSION\001") == 0) {
		fprintf(sockstream, "NOTICE %s :\001VERSION %s\001\r\n",
				prefix, VERSION);
		fflush(sockstream);
	}
}

/*
 * rejoins on kick
 */
void irc_command_kick(char *params[])
{
	if (!params[1])
		return;
	if (strcmp(params[1], ucfg_lookup_string(conf, "core:nick")) == 0)
		irc_join();
}

void irc_register(void)
{
	struct passwd pwd, *result;
	char *pwdsbuf;
	size_t pwdsbuf_size;
	int getpwret;
	char username[16];

	pwdsbuf_size = sysconf(_SC_GETPW_R_SIZE_MAX);
	if (pwdsbuf_size == -1) {
		pwdsbuf_size = 8192;
	}

	pwdsbuf = malloc(pwdsbuf_size);

	if ((getpwret = getpwuid_r(geteuid(), &pwd, pwdsbuf, pwdsbuf_size,
					&result)) || result == NULL) {
		error(0, getpwret, "Could not determine current username");
		snprintf(username, 16, "%s", "unknown");
	} else {
		snprintf(username, 16, "%s", result->pw_name);
	}

	free(pwdsbuf);

	fprintf(sockstream, "NICK %s\r\n",
			ucfg_lookup_string(conf, "core:nick"));
	fprintf(sockstream, "USER %s localhost localhost "
			":H-Vanira the Bot\r\n", username);
	fflush(sockstream);
}

void irc_join(void)
{
	fprintf(sockstream, "JOIN %s\r\n",
			ucfg_lookup_string(conf, "core:channel"));
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
