/*
 * Copyright © 2009  Sebastian Thorarensen
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <error.h>
#include <errno.h>
#include <netdb.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h> /* for compability with older systems */
#include <sys/socket.h>
/*#include <netinet/in.h>*/
/*#include <arpa/inet.h>*/

#define MASTER_OPERATOR "indigo176@titan.blinkenshell.org"
#define VERSION "H-Vanira (20091013) by InDigo176"

#define RECONNECTION_DELAY 30
#define DISCONNECTION_TIMEOUT 2


void read_opers(void);
void install_signals(void);
void handle_signal(int);
bool irc_connect(char *, char *);
void handle_forever(char *);
void read_command(char *);

int strscmp(const char *, const char *);

void irc_command_ping(char *);
void irc_command_join(char *);
void irc_command_privmsg(char *);
void irc_command_kick(char *);
void irc_join(void);


struct oper {
	char mask[128];
	struct oper *next;
};

char *irc_nick;
char *irc_channel;
struct oper opers;

char *outbuf;
int sockfd;
FILE *sockstream;

int main(int argc, char *argv[]) {
	int ci = 1;
	char *hostname;
	char *port;
	char *buf;

	if (argc < 4 || argc > 5)
		error(2, 0, "Wrong number of arguments");

	irc_nick = argv[ci++];
	irc_channel = argv[ci++];
	hostname = argv[ci++];
	if (argc > 4)
		port = argv[ci];
	else
		port = "6667";


	/* 128 is a good max value, we don't wanna risk creating IRC
	 * messages over 512 */
	if (strlen(irc_nick) > 128 || strlen(irc_channel) > 128)
		error(2, 0, "Nick or channel name too long");

	read_opers();

	buf = malloc(512 * sizeof (char));
	if (!buf)
		error(EXIT_FAILURE, 0, "Cannot allocate memory");

	install_signals();

	for (;;) {
		while (!irc_connect(hostname, port))
			sleep(RECONNECTION_DELAY);
		handle_forever(buf);
		printf("Disconnected!\n");
	}

	return EXIT_SUCCESS;
}

void read_opers(void) {
	FILE *maskf;
	struct oper *current = &opers;
	char c;

	maskf = fopen("opers", "r");
	if (!maskf) {
		error(0, errno, "Cannot read opers file");
		return;
	}

	strcpy(current->mask, MASTER_OPERATOR);

	/* peek for EOF so that we don't allocate one struct too much */
	while ((c=getc_unlocked(maskf)) != EOF) {
		int len;
		
		ungetc(c, maskf);

		current->next = malloc(sizeof (struct oper));
		if (!current->next)
			error(EXIT_FAILURE, 0, "Cannot allocate memory");
		current = current->next;

		len = ftell(maskf);
		fgets(current->mask, 128, maskf);
		len = ftell(maskf) - len - 1;
		current->mask[len] = '\0'; /* remove \n */
	}

	if (fclose(maskf) == EOF)
		perror("fclose");

}

void install_signals(void) {
	int signals[] = {SIGHUP, SIGINT, SIGSEGV, SIGTERM};
	size_t len = sizeof signals / sizeof (int);
	struct sigaction action;
	size_t i;

	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;

	for (i=0; i<len; i++) {
		if (sigaction(signals[i], &action, NULL) < 0) {
			perror("sigaction");
			exit(EXIT_FAILURE);
		}
	}
}

void handle_signal(int sig) {
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
		default:
			return;
	}

	if (sockstream) {
		fd_set sockset;
		struct timeval tv;

		fprintf(sockstream, "QUIT :%s\r\n", quitmsg);
		fflush(sockstream);

		FD_ZERO(&sockset);
		FD_SET(sockfd, &sockset);
		tv.tv_sec = DISCONNECTION_TIMEOUT;
		tv.tv_usec = 0;

		select(sockfd+1, &sockset, NULL, NULL, &tv);
	}

	exit(exitval);
}

bool irc_connect(char *hostname, char *port) {
	struct addrinfo hints;
	struct addrinfo *ai;
	int errcode;

	hints.ai_flags = 0;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;

	errcode = getaddrinfo(hostname, port, &hints, &ai);
	if (errcode < 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(errcode));
		return false;
	}

	sockfd = socket(ai->ai_family, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("socket");
		return false;
	}

	if (connect(sockfd, ai->ai_addr, ai->ai_addrlen) == -1) {
		perror("connect");
		freeaddrinfo(ai);
		return false;
	}

	sockstream = fdopen(sockfd, "w");
	if (!sockstream) {
		perror("fdopen");
		freeaddrinfo(ai);
		return false;
	}

	freeaddrinfo(ai);
	return true;

}

void handle_forever(char *buf) {
	size_t offset = 0;
	ssize_t rsize = 0;

	/* register */
	fprintf(sockstream, "NICK %s\r\n", irc_nick);
	fprintf(sockstream, "USER H-Vanira localhost localhost "
			":H-Vanira the Bot\r\n");
	fflush(sockstream);

	while ((rsize += read(sockfd, buf+offset, 512-offset))) {
		while (offset < (size_t)rsize) {
			if (!(buf[offset++] == '\r' && buf[offset] == '\n')) 
				continue; /* search for \r\n */

			buf[offset-1] = '\0';
			read_command(buf);
			offset++;
			if ((size_t)rsize == offset) {
				/* only got one command, goto read */
				rsize = 0;
				offset = 0;
				break;
			}

			/* more commands on the buffer
			 * move command to beginning of buffer and
			 * continue search for \r\n */
			rsize -= offset;
			memmove(buf, buf+offset, rsize);
			offset = 0;
			continue;
		}

		/* avoid infinite loop if we receive a message
		 * which is too long */
		if (offset == 512) 
			offset = 0;
	}
}

void read_command(char *msg) {
	char *cmd = msg;

	/*printf(msg);
	printf("\n");*/

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
			irc_command_privmsg(cmd+params);
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
int strscmp(const char *s1, const char *s2) {
	int i;
	for (i=0; s1[i]!=' '&&s1[i]!='\0'&&s2[i]!='\0'; i++) {
		if (s1[i] != s2[i])
			return -1;
	}
	return i + 1;
}

void irc_command_ping(char *params) {
        fprintf(sockstream, "PONG %s\r\n", params);
        fflush(sockstream);
}

/*
 * ops users in opers list on join
 */
void irc_command_join(char *prefix) {
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
        if (len > 128)
                return; /* mask to long for us */

        for (o=&opers; o; o=o->next) {
                if (memcmp(mask, o->mask, len) == 0) {
                        fprintf(sockstream, "MODE %s +o %s\r\n", irc_channel,
                                        prefix);
                        fflush(sockstream);
                        return;
                }
        }
}

void irc_command_privmsg(char *params) {
	char about[128+8+1]; /* nick: 128, command: 8, nullbyte: 1 */
	char *msg = strchr(params, ' ');
	if (!msg)
		return;
	msg[0] = '\0';

	msg++;

	if (strcmp(params, irc_channel) != 0)
		return;

	sprintf(about, ":%s: about", irc_nick);

	if (strcmp(msg, about) == 0) {
		fprintf(sockstream, "PRIVMSG %s :%s\r\n", irc_channel,
				VERSION);
		fflush(sockstream);
	}
}

/*
 * rejoins on kick
 */
void irc_command_kick(char *params) {
	char *end;

	params = strchr(params, ' ') + 1; /* jump to nickname */
	if (!params)
		return;
	end = strchr(params, ' ');
	end[0] = '\0';

	if (strcmp(params, irc_nick) == 0) {
		irc_join();
	}
}

void irc_join(void) {
	fprintf(sockstream, "JOIN %s\r\n", irc_channel);
	fflush(sockstream);
}
