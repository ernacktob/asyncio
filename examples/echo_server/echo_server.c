#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>

#include "asyncio_fdevents.h"
#include "asyncio_threadpool.h"

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
static void ConnectionState_readable(const struct asyncio_fdevents_loop *eventloop, int sockfd, const void *revinfo, void *arg, int *continued);
static void ConnectionState_writable(const struct asyncio_fdevents_loop *eventloop, int sockfd, const void *revinfo, void *arg, int *continued);
static void ConnectionState_connected(const struct asyncio_fdevents_loop *eventloop, struct ConnectionState *state, int sockfd);

static int create_accept_sock(void);
static void accept_clients(const struct asyncio_fdevents_loop *eventloop, int fd, const void *revinfo, void *arg, int *continued);
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

static void ConnectionState_readable(const struct asyncio_fdevents_loop *eventloop, int sockfd, const void *revinfo, void *arg, int *continued)
{
	struct ConnectionState *state;
	const struct asyncio_fdevents_poll_evinfo *pollrevinfo;
	struct asyncio_fdevents_poll_evinfo evinfo;
	char *newline;
	int sendit;
	struct asyncio_fdevents_handle *handle;
	ssize_t rb;

	pollrevinfo = revinfo;
	state = arg;

	if (pollrevinfo->events & (POLLERR | POLLHUP | POLLNVAL)) {
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
		evinfo.events = POLLOUT;

		if (eventloop->listen(eventloop, sockfd, &evinfo, ConnectionState_writable, state, ASYNCIO_THREADPOOL_FLAG_NONE, &handle) != 0) {
			fprintf(stderr, "Failed to register fdevent.\n");
			ConnectionState_destroy(state);
			close(sockfd);
			return;
		}

		handle->release(handle);
	} else {
		*continued = 1;
	}
}

static void ConnectionState_writable(const struct asyncio_fdevents_loop *eventloop, int sockfd, const void *revinfo, void *arg, int *continued)
{
	struct ConnectionState *state;
	const struct asyncio_fdevents_poll_evinfo *pollrevinfo;
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_fdevents_handle *handle;
	ssize_t sb;

	pollrevinfo = revinfo;
	state = arg;

	if (pollrevinfo->events & (POLLERR | POLLHUP | POLLNVAL)) {
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

		evinfo.events = POLLIN;

		if (eventloop->listen(eventloop, sockfd, &evinfo, ConnectionState_readable, state, ASYNCIO_THREADPOOL_FLAG_NONE, &handle) != 0) {
			fprintf(stderr, "Failed to register fdevent.\n");
			ConnectionState_destroy(state);
			close(sockfd);
			return;
		}

		handle->release(handle);
	} else {
		*continued = 1;
	}
}

static void ConnectionState_connected(const struct asyncio_fdevents_loop *eventloop, struct ConnectionState *state, int sockfd)
{
	const char *greeting = "Welcome to the echo server.\nType a line of at most 1000 characters, and it will be echoed back.\n\n";
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_fdevents_handle *handle;

	strncpy(state->echostr, greeting, MAX_STRLEN);
	state->echostr[MAX_STRLEN] = '\0';
	state->pos = 0;
	state->len = strlen(state->echostr);

	evinfo.events = POLLOUT;

	if (eventloop->listen(eventloop, sockfd, &evinfo, ConnectionState_writable, state, ASYNCIO_THREADPOOL_FLAG_NONE, &handle) != 0) {
		fprintf(stderr, "Failed to register fdevent.\n");
		return;
	}

	handle->release(handle);
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

static void accept_clients(const struct asyncio_fdevents_loop *eventloop, int fd, const void *revinfo, void *arg, int *continued)
{
	struct ConnectionState *state;
	int client_sock;
	struct sockaddr dummy_addr;
	socklen_t dummy_len;
	(void)revinfo;
	(void)arg;

	dummy_len = sizeof dummy_addr;

	client_sock = accept(fd, &dummy_addr, &dummy_len);

	while (client_sock > 0) {
		state = ConnectionState_create();

		if (state == NULL)
			fprintf(stderr, "Failed to create ConnectionState\n");
		else
			ConnectionState_connected(eventloop, state, client_sock);

		client_sock = accept(fd, &dummy_addr, &dummy_len);
	}

	if (errno != EWOULDBLOCK) {
		fprintf(stderr, "error during accept.\n");
		return;
	}

	*continued = 1;
}

int main()
{
	struct asyncio_fdevents_options options;
	struct asyncio_fdevents_loop *eventloop;
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_fdevents_handle *handle;
	int accept_sockfd;

	if (asyncio_fdevents_init() != 0) {
		fprintf(stderr, "Failed to init asyncio fdevents.\n");
		return -1;
	}

	accept_sockfd = create_accept_sock();

	if (accept_sockfd == -1) {
		fprintf(stderr, "Failed to create accept sock.\n");
		asyncio_fdevents_cleanup();
		return -1;
	}

	options.max_nfds = 10000;
	options.backend_type = ASYNCIO_FDEVENTS_BACKEND_POLL;

	if (asyncio_fdevents_eventloop(&options, &eventloop) != 0) {
		fprintf(stderr, "Failed to create eventloop.\n");
		close(accept_sockfd);
		asyncio_fdevents_cleanup();
		return -1;
	}

	evinfo.events = POLLIN;

	if (eventloop->listen(eventloop, accept_sockfd, &evinfo, accept_clients, NULL, ASYNCIO_THREADPOOL_FLAG_NONE, &handle) != 0) {
		fprintf(stderr, "Failed to listen for fdevent.\n");
		eventloop->release(eventloop);
		close(accept_sockfd);
		asyncio_fdevents_cleanup();
		return -1;
	}

	/* Should never return, server runs forever */
	if (handle->wait(handle) != 0) {
		fprintf(stderr, "Failed to wait handle.\n");
		eventloop->release(eventloop);
		close(accept_sockfd);
		asyncio_fdevents_cleanup();
		return -1;
	}

	handle->release(handle);
	eventloop->release(eventloop);
	close(accept_sockfd);
	asyncio_fdevents_cleanup();

	return 0;
}
