#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "asyncio_fdevents.h"
#include "asyncio_threadpool.h"

/* PROTOTYPES */
static void printf_locked(const char *fmt, ...);
static int create_accept_sock(void);
static void on_read(const struct asyncio_fdevents_callback_info *info, int *continued);
/* END PROTOTYPES */

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

	if (listen(accept_sock, 1) != 0) {
		perror("listen");
		close(accept_sock);
		return -1;
	}

	return accept_sock;
}

static void on_read(const struct asyncio_fdevents_callback_info *info, int *continued)
{
	const struct asyncio_fdevents_poll_evinfo *pollrevinfo;
	int client_sock;
	struct sockaddr dummy_addr;
	socklen_t dummy_len;
	char byte;

	pollrevinfo = info->revinfo;
	dummy_len = sizeof dummy_addr;
	printf_locked("on_read: revents = %hd, arg = %p\n", pollrevinfo->events, info->arg);

	client_sock = accept(info->fd, &dummy_addr, &dummy_len);

	if (client_sock < 0) {
		perror("accept");
		return;
	}

	printf_locked("Accepted new client!\n");
	send(client_sock, "HELLO WORLD\n", strlen("HELLO WORLD\n"), 0);

	/* This is needed on OS X because it sets sockets returned from accept
	 * with a nonblocking accept_sock as nonblocking as well.
	 * We're setting to to blocking just to see how it will behave if it gets
	 * stuck in here while waiting for a client to type something. It's a good
	 * test for cancellations. */
	if (asyncio_fdevents_set_blocking(client_sock) != 0)
		printf_locked("Failed to set client sock to blocking.\n");

	recv(client_sock, &byte, 1, 0); /* This should block because we didn't set the client_sock to nonblocking. */
	close(client_sock);

	*continued = 1;
}

int main()
{
	struct asyncio_fdevents_options options;
	struct asyncio_fdevents_loop *eventloop;

	struct asyncio_fdevents_listen_info listen_info;
	struct asyncio_fdevents_poll_evinfo evinfo;
	struct asyncio_fdevents_handle *handle;

	int sockfd;

	sockfd = create_accept_sock();

	if (sockfd < 0) {
		printf_locked("Failed to create accept sock.\n");
		exit(EXIT_FAILURE);
	}

	if (asyncio_fdevents_init() != 0) {
		printf_locked("Failed to initialize fdevents module.\n");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	options.max_nfds = 10000;
	options.backend_type = ASYNCIO_FDEVENTS_BACKEND_POLL;

	if (asyncio_fdevents_eventloop(&options, &eventloop) != 0) {
		printf_locked("Failed to create eventloop.\n");
		asyncio_fdevents_cleanup();
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	evinfo.events = POLLIN;

	ASYNCIO_FDEVENTS_LISTEN_INFO_DEFAULT_INIT(listen_info, sockfd, &evinfo, on_read);
	listen_info.threadpool_flags = ASYNCIO_THREADPOOL_FLAG_CANCELLABLE;

	if (eventloop->listen(eventloop, &listen_info, &handle) != 0) {
		printf_locked("Failed to listen on eventloop.\n");
		eventloop->release(eventloop);
		asyncio_fdevents_cleanup();
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

	printf_locked("Releasing handle.\n");
	handle->release(handle);
	printf_locked("release eventloop.\n");
	eventloop->release(eventloop);
	printf_locked("cleanup fdevents\n");
	asyncio_fdevents_cleanup();

	close(sockfd);
	return 0;
}
