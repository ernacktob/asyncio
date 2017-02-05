#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>

#include "asyncio.h"

#define LOCALHOST	"127.0.0.1"
#define PORT		12345
#define BACKLOG_SIZE	1

#define MAX_STRLEN	1000

/* STRUCT DEFINITIONS */
struct ConnectionState {
	char echostr[MAX_STRLEN];
	size_t len;
	size_t pos;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static struct ConnectionState *ConnectionState_create(void);
static void ConnectionState_destroy(struct ConnectionState *state);
static void ConnectionState_readable(int sockfd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void ConnectionState_writable(int sockfd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
static void ConnectionState_connected(struct ConnectionState *state, int sockfd);

static int create_accept_sock(void);
static void accept_clients(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued);
/* END PROTOTYPES */

static struct ConnectionState *ConnectionState_create(void)
{
	struct ConnectionState *state;

	state = malloc(sizeof *state);

	if (state == NULL)
		return NULL;

	state->echostr[0] = '\0';
	state->len = 0;
	state->pos = 0;

	return state;
}

static void ConnectionState_destroy(struct ConnectionState *state)
{
	free(state);
}

static void ConnectionState_readable(int sockfd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct ConnectionState *state;
	char *newline;
	int sendit;
	asyncio_handle_t handle;
	ssize_t rb;

	state = (struct ConnectionState *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		ConnectionState_destroy(state);
		close(sockfd);
		return;
	}

	rb = recv(sockfd, state->echostr + state->pos, MAX_STRLEN - state->pos, 0);

	if (rb == -1 || rb == 0) {
		ConnectionState_destroy(state);
		close(sockfd);
		return;
	}

	sendit = 0;
	newline = strchr(state->echostr, '\n');

	if (newline) {
		state->len = (size_t)(newline - state->echostr) + 1;
		state->pos = 0;
		sendit = 1;
	} else if (state->pos + rb >= MAX_STRLEN) {
		state->len = MAX_STRLEN;
		state->pos = 0;
		state->echostr[MAX_STRLEN - 1] = '\n';
		sendit = 1;
	}

	if (sendit) {
		if (asyncio_fdevent(sockfd, ASYNCIO_FDEVENT_WRITE, ConnectionState_writable, state, ASYNCIO_FLAG_NONE, &handle) != 0) {
			fprintf(stderr, "Failed to register fdevent.\n");
			ConnectionState_destroy(state);
			close(sockfd);
			return;
		}

		asyncio_release(handle);
	} else {
		asyncio_continue(continued);
	}
}

static void ConnectionState_writable(int sockfd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct ConnectionState *state;
	asyncio_handle_t handle;
	ssize_t sb;

	state = (struct ConnectionState *)arg;

	if (revents & ASYNCIO_FDEVENT_ERROR) {
		ConnectionState_destroy(state);
		close(sockfd);
		return;
	}

	sb = send(sockfd, state->echostr + state->pos, state->len - state->pos, 0);

	if (sb == -1) {
		ConnectionState_destroy(state);
		close(sockfd);
		return;
	}

	state->pos += sb;

	if (state->pos == state->len) {
		state->echostr[0] = '\0';
		state->len = 0;
		state->pos = 0;

		if (asyncio_fdevent(sockfd, ASYNCIO_FDEVENT_READ, ConnectionState_readable, state, ASYNCIO_FLAG_NONE, &handle) != 0) {
			fprintf(stderr, "Failed to register fdevent.\n");
			ConnectionState_destroy(state);
			close(sockfd);
			return;
		}

		asyncio_release(handle);
	} else {
		asyncio_continue(continued);
	}
}

static void ConnectionState_connected(struct ConnectionState *state, int sockfd)
{
	const char *greeting = "Welcome to the echo server.\nType a line of at most 1000 characters, and it will be echoed back.\n\n";
	asyncio_handle_t handle;

	strncpy(state->echostr, greeting, MAX_STRLEN);
	state->echostr[MAX_STRLEN] = '\0';
	state->pos = 0;
	state->len = strlen(state->echostr);

	if (asyncio_fdevent(sockfd, ASYNCIO_FDEVENT_WRITE, ConnectionState_writable, state, ASYNCIO_FLAG_NONE, &handle) != 0) {
		fprintf(stderr, "Failed to register fdevent.\n");
		return;
	}

	asyncio_release(handle);
}

static int create_accept_sock(void)
{
	int accept_sock;
	struct sockaddr_in my_addr;
	int one = 1;

	accept_sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (accept_sock == -1) {
		perror("socket");
		return -1;
	}

	/* Set REUSEADDR on accept_sock to be able to bind to address next time we run immediately. */
	if (setsockopt(accept_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
		perror("setsockopt");
		close(accept_sock);
		return -1;
	}

	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(PORT);

	if (inet_aton(LOCALHOST, &my_addr.sin_addr) != 1) {
		perror("inet_aton");
		close(accept_sock);
		return -1;
	}

	memset(my_addr.sin_zero, 0, sizeof my_addr.sin_zero);

	if (bind(accept_sock, (struct sockaddr *)&my_addr, sizeof my_addr) != 0) {
		perror("bind");
		close(accept_sock);
		return -1;
	}

	if (listen(accept_sock, BACKLOG_SIZE) != 0) {
		perror("listen");
		close(accept_sock);
		return -1;
	}

	return accept_sock;
}

static void accept_clients(int fd, asyncio_fdevent_t revents, void *arg, asyncio_continue_t continued)
{
	struct ConnectionState *state;
	int client_sock;
	struct sockaddr dummy_addr;
	socklen_t dummy_len;
	(void)revents;
	(void)arg;

	dummy_len = sizeof dummy_addr;

	client_sock = accept(fd, &dummy_addr, &dummy_len);

	while (client_sock > 0) {
		state = ConnectionState_create();

		if (state == NULL)
			fprintf(stderr, "Failed to create ConnectionState\n");
		else
			ConnectionState_connected(state, client_sock);

		client_sock = accept(fd, &dummy_addr, &dummy_len);
	}

	if (errno != EWOULDBLOCK) {
		fprintf(stderr, "error during accept.\n");
		return;
	}

	asyncio_continue(continued);
}

int main()
{
	asyncio_handle_t handle;
	int accept_sockfd;

	accept_sockfd = create_accept_sock();

	if (accept_sockfd == -1) {
		fprintf(stderr, "Failed to create accept sock.\n");
		return -1;
	}

	if (asyncio_fdevent(accept_sockfd, ASYNCIO_FDEVENT_READ, accept_clients, NULL, ASYNCIO_FLAG_NONE, &handle) != 0) {
		fprintf(stderr, "Failed to register fdevent.\n");
		close(accept_sockfd);
		return -1;
	}

	/* Should never return, server runs forever */
	if (asyncio_join(handle) != 0) {
		fprintf(stderr, "Failed to join handle.\n");
		close(accept_sockfd);
		return -1;
	}

	asyncio_release(handle);
	close(accept_sockfd);

	return 0;
}
