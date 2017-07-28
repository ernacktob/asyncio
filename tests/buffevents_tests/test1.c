#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>

#include "asyncio_buffevents.h"
#include "asyncio_fdevents.h"
#include "asyncio_threadpool.h"

#define LOCALHOST	"127.0.0.1"
#define PORT		12345
#define BACKLOG_SIZE	1

#define MAX_STRLEN	1000

/* XXX Idea: merge the read and write of buffevents, pass buf as const uint8_t *, remove buf_to_free, and force user to use arg for freeing stuff. */
/* STRUCT DEFINITIONS */
struct ConnectionState {
	char read_buffer[MAX_STRLEN];
	const struct asyncio_fdevents_loop *eventloop;
	int client_sock;
};
/* END STRUCT DEFINITIONS */

/* PROTOTYPES */
static struct ConnectionState *ConnectionState_create(const struct asyncio_fdevents_loop *eventloop, int client_sock);
static void ConnectionState_destroy(struct ConnectionState *state);

static int is_line(const uint8_t *buf, size_t len, void *arg);
static int create_accept_sock(void);

static int check_evinfo_errors(const void *revinfo);
static int client_read(int fd, uint8_t *buf, size_t len, size_t *rdlen, void *arg);
static int client_write(int fd, const uint8_t *buf, size_t len, size_t *wrlen, void *arg);
static void client_error(void *arg);

static void client_read_done(uint8_t *buf, size_t len, void *arg);
static void client_write_done(uint8_t *buf, size_t len, void *arg);
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

	state->read_buffer[0] = '\0';
	state->eventloop = eventloop;
	state->client_sock = client_sock;

	return state;
}

static void ConnectionState_destroy(struct ConnectionState *state)
{
	/* Don't release eventloop, we have only 1 reference and it is kept until main returns... */
	close(state->client_sock);
	free(state);
}

static int is_line(const uint8_t *buf, size_t len, void *arg)
{
	(void)arg;
	return buf[len - 1] == '\n';
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

static int check_evinfo_errors(const void *revinfo)
{
	const struct asyncio_fdevents_poll_evinfo *pollrevinfo;

	pollrevinfo = revinfo;
	return pollrevinfo->events & (POLLERR | POLLHUP | POLLNVAL);
}

static int client_read(int fd, uint8_t *buf, size_t len, size_t *rdlen, void *arg)
{
	ssize_t rb;
	(void)arg;
	(void)len;

	rb = recv(fd, buf, 1, 0);

	if (rb < 0) {
		perror("recv");
		return -1;
	} else if (rb == 0) {
		/* Connection closed? XXX handle separately? */
		return -1;
	}

	*rdlen = 1;
	return 0;
}

static int client_write(int fd, const uint8_t *buf, size_t len, size_t *wrlen, void *arg)
{
	ssize_t sb;
	(void)arg;

	sb = send(fd, buf, len, 0);

	if (sb < 0) {
		perror("send");
		return -1;
	}

	*wrlen = sb;
	return 0;
}

static void client_error(void *arg)
{
	struct ConnectionState *state;

	state = arg;
	ConnectionState_destroy(state);
}

static void client_read_done(uint8_t *buf, size_t len, void *arg)
{
	struct ConnectionState *state;
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_buffevents_write_info write_info;
	struct asyncio_buffevents_handle *handle;

	state = arg;

	evinfo.events = POLLOUT;
	write_info.eventloop = state->eventloop;
	write_info.evinfo = &evinfo;
	write_info.fd = state->client_sock;
	write_info.buf = buf;
	write_info.len = len;
	write_info.arg = state;
	write_info.buf_to_free = NULL;
	write_info.threadpool_flags = ASYNCIO_THREADPOOL_FLAG_NONE;
	write_info.is_errored = check_evinfo_errors;
	write_info.write_cb = client_write;
	write_info.error_cb = client_error;
	write_info.cancelled_cb = NULL;
	write_info.done_cb = client_write_done;

	if (asyncio_buffevents_write(&write_info, &handle) != 0) {
		fprintf(stderr, "Failed to recv buffer to read.\n");
		ConnectionState_destroy(state);
		return;
	}

	handle->release(handle);
}

static void client_write_done(uint8_t *buf, size_t len, void *arg)
{
	struct ConnectionState *state;
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_buffevents_read_info read_info;
	struct asyncio_buffevents_handle *handle;
	(void)buf;
	(void)len;

	state = arg;
	evinfo.events = POLLIN;

	read_info.eventloop = state->eventloop;
	read_info.evinfo = &evinfo;
	read_info.fd = state->client_sock;
	read_info.buf = (uint8_t *)(state->read_buffer);
	read_info.len = MAX_STRLEN;
	read_info.arg = state;
	read_info.threadpool_flags = ASYNCIO_THREADPOOL_FLAG_NONE;
	read_info.is_errored = check_evinfo_errors;
	read_info.read_cb = client_read;
	read_info.until_cb = is_line;
	read_info.error_cb = client_error;
	read_info.cancelled_cb = NULL;
	read_info.done_cb = client_read_done;

	if (asyncio_buffevents_read(&read_info, &handle) != 0) {
		fprintf(stderr, "Failed to recv buffer to read.\n");
		ConnectionState_destroy(state);
		return;
	}

	handle->release(handle);
}

static void client_connected(struct ConnectionState *state)
{
	const char *greeting = "Welcome to the echo server.\nType a line of at most 1000 characters, and it will be echoed back.\n\n";
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_buffevents_write_info write_info;
	struct asyncio_buffevents_handle *handle;

	evinfo.events = POLLOUT;
	write_info.eventloop = state->eventloop;
	write_info.evinfo = &evinfo;
	write_info.fd = state->client_sock;
	write_info.buf = (const uint8_t *)greeting;
	write_info.len = strlen(greeting);
	write_info.arg = state;
	write_info.buf_to_free = NULL;
	write_info.threadpool_flags = ASYNCIO_THREADPOOL_FLAG_NONE;
	write_info.is_errored = check_evinfo_errors;
	write_info.write_cb = client_write;
	write_info.error_cb = client_error;
	write_info.cancelled_cb = NULL;
	write_info.done_cb = client_write_done;

	if (asyncio_buffevents_write(&write_info, &handle) != 0) {
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
