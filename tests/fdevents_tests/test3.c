#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/errno.h>

#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>

#include <pthread.h>

#include "asyncio_fdevents.h"
#include "asyncio_threadpool.h"

#define CONNECTIONS_PER_SECOND		1000
#define MAX_CONCURRENT_CONNECTIONS	1000
#define ACCEPT_LATENCY_MS		5
#define BACKLOG_SIZE			((((CONNECTIONS_PER_SECOND) * (ACCEPT_LATENCY_MS)) / 1000) * 2)

/* PROTOTYPES */
static void printf_locked(const char *fmt, ...);
static int create_accept_sock(void);

static void on_readable(const struct asyncio_fdevents_callback_info *info, int *continued);
static void on_writable(const struct asyncio_fdevents_callback_info *info, int *continued);
static void on_connect(const struct asyncio_fdevents_callback_info *info, int *continued);
/* END PROTOTYPES */

/* GLOBALS */
static unsigned int count = 0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
/*END GLOBALS */

static void printf_locked(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	flockfile(stderr);
	vfprintf(stderr, fmt, args);
	fflush(stderr);
	funlockfile(stderr);
	va_end(args);
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

	if (asyncio_fdevents_set_nonblocking(accept_sock) != 0) {
		printf_locked("Failed to set accept_sock to nonblocking.\n");
		close(accept_sock);
		return -1;
	}

/*	my_addr.sin_len = sizeof my_addr; */
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(12345);

	if (inet_aton("127.0.0.1", &my_addr.sin_addr) != 1) {
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

static void on_readable(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	char byte;
	ssize_t rb;

	rb = recv(info->fd, &byte, sizeof byte, 0);

	if (rb < 0) {
		printf_locked("recv failed\n");
		perror("recv");
		close(info->fd);
		*continued = 0;
		return;
	}

	if (rb == 0) {
		close(info->fd);
		*continued = 0;
		return;
	}

	if (byte != 'a')
		printf_locked("Did not receive correct byte.\n");

	close(info->fd);
	*continued = 0;
}

static void on_writable(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	struct asyncio_fdevents_select_evinfo evinfo;
	struct asyncio_fdevents_handle *handle;
	struct asyncio_fdevents_listen_info listen_info;
	ssize_t sb;

	sb = send(info->fd, "HELLO WORLD\n", strlen("HELLO WORLD\n"), 0);

	if (sb < 0) {
		printf_locked("send failed\n");
		close(info->fd);
		*continued = 0;
		return;
	}

	evinfo.events = ASYNCIO_FDEVENTS_SELECT_READABLE;
	ASYNCIO_FDEVENTS_LISTEN_INFO_DEFAULT_INIT(listen_info, info->fd, &evinfo, on_readable);

	if (info->eventloop->listen(info->eventloop, &listen_info, &handle) != 0) {
		printf_locked("Failed to register on_readable.\n");
		close(info->fd);
		*continued = 0;
		return;
	}

	handle->release(handle);
	*continued = 0;
}

static void on_connect(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	struct asyncio_fdevents_select_evinfo evinfo;
	struct asyncio_fdevents_handle *handle;
	struct asyncio_fdevents_listen_info listen_info;
	int client_sock;
	struct sockaddr dummy_addr;
	socklen_t dummy_len;

	dummy_len = sizeof dummy_addr;

	client_sock = accept(info->fd, &dummy_addr, &dummy_len);

	while (client_sock > 0) {
		if (asyncio_fdevents_set_nonblocking(client_sock) != 0) {
			printf_locked("Failed to set client sock to nonblocking.\n");
			close(client_sock);
			break;
		}

		evinfo.events = ASYNCIO_FDEVENTS_SELECT_WRITABLE;
		ASYNCIO_FDEVENTS_LISTEN_INFO_DEFAULT_INIT(listen_info, client_sock, &evinfo, on_writable);

		if (info->eventloop->listen(info->eventloop, &listen_info, &handle) != 0) {
			printf_locked("Failed to register on_writable.\n");
			close(client_sock);
		} else {
			handle->release(handle);

			pthread_mutex_lock(&mtx);
			++count;
			pthread_mutex_unlock(&mtx);
		}

		client_sock = accept(info->fd, &dummy_addr, &dummy_len);
	}

	if (errno != EWOULDBLOCK) {
		perror("accept");
		printf_locked("error during accept.\n");
		*continued = 0;
		return;
	}

	*continued = 1;
}

int main()
{
	struct rlimit rl;
	struct asyncio_fdevents_options options;
	struct asyncio_fdevents_loop *eventloop;
	struct asyncio_fdevents_select_evinfo evinfo;
	struct asyncio_fdevents_listen_info listen_info;
	struct asyncio_fdevents_handle *handle;
	int sockfd;

	if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
		printf_locked("Failed to get rlimit for open files.\n");
		exit(EXIT_FAILURE);
	}

	rl.rlim_cur = MAX_CONCURRENT_CONNECTIONS + 10;

	if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
		printf_locked("Failed to set rlimit for open files.\n");
		exit(EXIT_FAILURE);
	}

	sockfd = create_accept_sock();

	if (sockfd < 0) {
		printf_locked("Failed to create accept sock.\n");
		exit(EXIT_FAILURE);
	}

	options.max_nfds = 1010;
	options.backend_type = ASYNCIO_FDEVENTS_BACKEND_SELECT;

	if (asyncio_fdevents_eventloop(&options, &eventloop) != 0) {
		printf_locked("Failed to create fdevents eventloop.\n");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	evinfo.events = ASYNCIO_FDEVENTS_SELECT_READABLE;

	ASYNCIO_FDEVENTS_LISTEN_INFO_DEFAULT_INIT(listen_info, sockfd, &evinfo, on_connect);
	listen_info.threadpool_flags = ASYNCIO_THREADPOOL_FLAG_CANCELLABLE;

	if (eventloop->listen(eventloop, &listen_info, &handle) != 0) {
		printf_locked("Failed to listen on event.\n");
		eventloop->release(eventloop);
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	printf_locked("Running for 10s...\n");
	usleep(10000000);

	printf_locked("Cancelling fdevent...\n");
	if (handle->cancel(handle) != 0)
		printf_locked("Failed to cancel.\n");

	printf_locked("Waiting for fdevent to complete...\n");
	if (handle->wait(handle) != 0)
		printf_locked("Failed to join.\n");

	printf_locked("count = %u\n", count);
	handle->release(handle);
	eventloop->release(eventloop);
	close(sockfd);
	return 0;
}
