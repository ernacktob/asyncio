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

#include "fdevents.h"

#define CONNECTIONS_PER_SECOND		5000
#define MAX_CONCURRENT_CONNECTIONS	5000
#define ACCEPT_LATENCY_MS		2
#define BACKLOG_SIZE			((((CONNECTIONS_PER_SECOND) * (ACCEPT_LATENCY_MS)) / 1000) * 2)

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

static void on_readable(int fd, uint16_t revents, void *arg, int *continued)
{
	char byte;
	ssize_t rb;
	(void)revents;
	(void)arg;
	(void)continued;

	rb = recv(fd, &byte, sizeof byte, 0);

	if (rb < 0) {
		printf_locked("recv failed\n");
		close(fd);
		return;
	}

	if (rb == 0) {
		close(fd);
		return;
	}

	if (byte != 'a')
		printf_locked("Did not receive correct byte.\n");

	close(fd);
}

static void on_writable(int fd, uint16_t revents, void *arg, int *continued)
{
	struct fdevent_info evinfo;
	fdevent_handle_t handle;
	ssize_t sb;
	(void)revents;
	(void)arg;
	(void)continued;

	sb = send(fd, "HELLO WORLD\n", strlen("HELLO WORLD\n"), 0);

	if (sb < 0) {
		printf_locked("send failed\n");
		close(fd);
		return;
	}

	evinfo.fd = fd;
	evinfo.events = FDEVENT_EVENT_READ;
	evinfo.flags = FDEVENT_FLAG_NONE;
	evinfo.cb = on_readable;
	evinfo.arg = NULL;

	if (fdevent_register(&evinfo, &handle) != 0) {
		printf_locked("Failed to register on_readable.\n");
		close(fd);
		return;
	}

	fdevent_release_handle(handle);
}

#include <pthread.h>

static unsigned int count = 0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void on_connect(int fd, uint16_t revents, void *arg, int *continued)
{
	struct fdevent_info evinfo;
	fdevent_handle_t handle;
	int client_sock;
	struct sockaddr dummy_addr;
	socklen_t dummy_len;
	(void)revents;
	(void)arg;

	dummy_len = sizeof dummy_addr;

	client_sock = accept(fd, &dummy_addr, &dummy_len);

	while (client_sock > 0) {
		evinfo.fd = client_sock;
		evinfo.events = FDEVENT_EVENT_WRITE;
		evinfo.flags = FDEVENT_FLAG_NONE;
		evinfo.cb = on_writable;
		evinfo.arg = NULL;

		if (fdevent_register(&evinfo, &handle) != 0) {
			printf_locked("Failed to register on_writable.\n");
			close(client_sock);
		}

		fdevent_release_handle(handle);

		pthread_mutex_lock(&mtx);
		++count;
		pthread_mutex_unlock(&mtx);

		client_sock = accept(fd, &dummy_addr, &dummy_len);
	}

	if (errno != EWOULDBLOCK) {
		printf_locked("error during accept.\n");
		return;
	}

	fdevent_continue(continued);
}

int main()
{
	struct rlimit rl;
	struct fdevent_info evinfo;
	fdevent_handle_t handle;
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

	evinfo.fd = sockfd;
	evinfo.events = FDEVENT_EVENT_READ;
	evinfo.flags = FDEVENT_FLAG_NONE;
	evinfo.cb = on_connect;
	evinfo.arg = NULL;

	if (fdevent_register(&evinfo, &handle) != 0) {
		printf_locked("Failed to wait event.\n");
		close(sockfd);
		exit(EXIT_FAILURE);
	}

	printf_locked("Running for 10s...\n");
	usleep(10000000);

	printf_locked("Cancelling fdevent...\n");
	if (fdevent_cancel(handle) != 0)
		printf_locked("Failed to cancel.\n");

	printf_locked("Waiting for fdevent to complete...\n");
	if (fdevent_join(handle) != 0)
		printf_locked("Failed to join.\n");

	printf_locked("count = %u\n", count);
	fdevent_release_handle(handle);
	close(sockfd);
	return 0;
}
