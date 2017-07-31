#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>

#include "asyncio_streams.h"
#include "asyncio_fdevents.h"
#include "asyncio_threadpool.h"

#define LOCALHOST	"127.0.0.1"
#define PORT		12345
#define BACKLOG_SIZE	1

#define MAX_BUFLEN	1000

/* STRUCT DEFINITIONS */
struct ConnectionState {
	char read_buffer[MAX_BUFLEN];
	char write_buffer[MAX_BUFLEN];
	size_t read_pos;
	struct asyncio_fdevents_poll_evinfo read_evinfo;
	struct asyncio_fdevents_poll_evinfo write_evinfo;
	struct asyncio_streams_policy policy;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static struct ConnectionState *ConnectionState_create(const struct asyncio_fdevents_loop *eventloop, int client_sock);
static void ConnectionState_destroy(struct ConnectionState *state);

static int has_newline(size_t len, uint8_t **unread_buf, size_t *unread_len, void *state);
static int create_accept_sock(void);

static int check_evinfo_errors(const void *revinfo, void *state);
static int client_read(uint8_t *buf, size_t len, size_t *rdlen, void *state);
static int client_write(const uint8_t *buf, size_t len, size_t *wrlen, void *state);
static void client_closed(void *state);
static void client_error(void *state);

static void client_read_done(size_t len, void *state);
static void client_write_done(void *state);
static void client_connected(struct ConnectionState *state);
static void accept_clients(const struct asyncio_fdevents_callback_info *info, int *continued);
/* END PROTOTYPES */

static struct ConnectionState *ConnectionState_create(const struct asyncio_fdevents_loop *eventloop, int client_sock)
{
	struct ConnectionState *state;

	state = malloc(sizeof *state);

	if (state == NULL) {
		perror("malloc");
		return NULL;
	}

	state->read_pos = 0;

	state->read_evinfo.events = POLLIN;
	state->write_evinfo.events = POLLOUT;

	state->policy.eventloop = eventloop;
	state->policy.read_evinfo = &state->read_evinfo;
	state->policy.write_evinfo = &state->write_evinfo;

	state->policy.state = state;

	state->policy.fd = client_sock;
	state->policy.threadpool_flags = ASYNCIO_THREADPOOL_FLAG_NONE;

	state->policy.is_errored = check_evinfo_errors;

	state->policy.read_cb = client_read;
	state->policy.write_cb = client_write;

	state->policy.until_cb = has_newline;

	state->policy.eof_cb = client_closed;
	state->policy.error_cb = client_error;
	state->policy.cancelled_cb = NULL;

	return state;
}

static void ConnectionState_destroy(struct ConnectionState *state)
{
	/* Don't release eventloop, we have only 1 reference and it is kept until main returns... */
	close(state->policy.fd);
	free(state);
}

static int has_newline(size_t len, uint8_t **unread_buf, size_t *unread_len, void *cstate)
{
	struct ConnectionState *state;
	uint8_t *newline;
	size_t linelen;

	state = cstate;

	newline = memchr(state->read_buffer, '\n', len);

	if (newline != NULL) {
		linelen = newline - (uint8_t *)(state->read_buffer);

		/* linelen doesn't include the newline */
		if (linelen < len - 1) {
			*unread_len = len - linelen - 1;
			*unread_buf = (uint8_t *)(state->read_buffer);
			state->read_pos = *unread_len;
		} else {
			*unread_len = 0;
			*unread_buf = NULL;
			state->read_pos = 0;
		}

		return 1;
	}

	*unread_len = 0;
	*unread_buf = NULL;
	state->read_pos = 0;

	return 0;
}

static int create_accept_sock()
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

	if (asyncio_fdevents_set_nonblocking(accept_sock) != 0) {
		fprintf(stderr, "Failed to set accept_sock nonblocking.\n");
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

static int check_evinfo_errors(const void *revinfo, void *state)
{
	const struct asyncio_fdevents_poll_evinfo *pollrevinfo;
	(void)state;

	pollrevinfo = revinfo;
	return pollrevinfo->events & (POLLERR | POLLHUP | POLLNVAL);
}

static int client_read(uint8_t *buf, size_t len, size_t *rdlen, void *cstate)
{
	struct ConnectionState *state;
	ssize_t rb;

	state = cstate;
	rb = recv(state->policy.fd, buf, len, 0);

	if (rb < 0) {
		perror("recv");
		return -1;
	} else if (rb == 0) {
		/* Connection closed -> EOF */
		*rdlen = 0;
		return 0;
	}

	*rdlen = rb;
	return 0;
}

static int client_write(const uint8_t *buf, size_t len, size_t *wrlen, void *cstate)
{
	struct ConnectionState *state;
	ssize_t sb;

	state = cstate;
	sb = send(state->policy.fd, buf, len, 0);

	if (sb < 0) {
		perror("send");
		return -1;
	}

	*wrlen = sb;
	return 0;
}

static void client_closed(void *cstate)
{
	struct ConnectionState *state;

	state = cstate;
	ConnectionState_destroy(state);
}

static void client_error(void *cstate)
{
	struct ConnectionState *state;

	state = cstate;
	ConnectionState_destroy(state);
}

static void client_read_done(size_t len, void *cstate)
{
	struct ConnectionState *state;
	struct asyncio_streams_handle *handle;

	state = cstate;

	memcpy(state->write_buffer, state->read_buffer, len);

	if (asyncio_streams_write((uint8_t *)(state->write_buffer), len, client_write_done, &state->policy, &handle) != 0) {
		fprintf(stderr, "Failed to register buffer to write.\n");
		ConnectionState_destroy(state);
		return;
	}

	handle->release(handle);
}

static void client_write_done(void *cstate)
{
	struct ConnectionState *state;
	struct asyncio_streams_handle *handle;

	state = cstate;

	/* We might already have things in the read_buffer because of the has_newline callback */
	if (asyncio_streams_read((uint8_t *)(state->read_buffer + state->read_pos), MAX_BUFLEN - state->read_pos, client_read_done, &state->policy, &handle) != 0) {
		fprintf(stderr, "Failed to register buffer to read.\n");
		ConnectionState_destroy(state);
		return;
	}

	handle->release(handle);
}

static void client_connected(struct ConnectionState *state)
{
	const char *greeting = "Welcome to the echo server.\nType a line of any length, and it will be echoed back.\n\n";
	struct asyncio_streams_handle *handle;

	if (asyncio_streams_write((const uint8_t *)greeting, strlen(greeting), client_write_done, &state->policy, &handle) != 0) {
		fprintf(stderr, "Failed to send buffer to write.\n");
		ConnectionState_destroy(state);
		return;
	}

	handle->release(handle);
}

static void accept_clients(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	struct ConnectionState *state;
	int client_sock;
	struct sockaddr dummy_addr;
	socklen_t dummy_len;

	dummy_len = sizeof dummy_addr;

	client_sock = accept(info->fd, &dummy_addr, &dummy_len);

	while (client_sock > 0) {
		if (asyncio_fdevents_set_nonblocking(client_sock) != 0) {
			fprintf(stderr, "Failed to set client_sock nonblocking.\n");
			close(client_sock);
			return;
		}

		state = ConnectionState_create(info->eventloop, client_sock);

		if (state == NULL)
			fprintf(stderr, "Failed to create connection state.\n");
		else
			client_connected(state);

		client_sock = accept(info->fd, &dummy_addr, &dummy_len);
	}

	if (errno != EWOULDBLOCK) {
		fprintf(stderr, "error during accept.\n");
		return;
	}

	*continued = 1;
}

int main()
{
	struct asyncio_fdevents_loop *eventloop;
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_fdevents_listen_info listen_info;
	struct asyncio_fdevents_handle *handle;
	int accept_sockfd;

	accept_sockfd = create_accept_sock();

	if (accept_sockfd == -1) {
		fprintf(stderr, "Failed to create accept sock.\n");
		return -1;
	}

	if (asyncio_fdevents_eventloop(10000, ASYNCIO_FDEVENTS_BACKEND_POLL, NULL, &eventloop) != 0) {
		fprintf(stderr, "Failed to create eventloop.\n");
		close(accept_sockfd);
		return -1;
	}

	evinfo.events = POLLIN;
	ASYNCIO_FDEVENTS_LISTEN_INFO_DEFAULT_INIT(listen_info, accept_sockfd, &evinfo, accept_clients);

	if (eventloop->listen(eventloop, &listen_info, &handle) != 0) {
		fprintf(stderr, "Failed to listen for fdevent.\n");
		eventloop->release(eventloop);
		close(accept_sockfd);
		return -1;
	}

	/* Should never return, server runs forever */
	if (handle->wait(handle) != 0) {
		fprintf(stderr, "Failed to wait handle.\n");
		eventloop->release(eventloop);
		close(accept_sockfd);
		return -1;
	}

	handle->release(handle);
	eventloop->release(eventloop);
	close(accept_sockfd);

	return 0;
}
