#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

/* PROTOTYPES */
void *do_stuff(void *arg);
/* END PROTOTYPES */

void *do_stuff(void *arg)
{
	int i;
	(void)arg;

	for (i = 0; i < 10; i++)
		printf("do_stuff: %d\n", i);

	printf("do_stuff: sleeping...\n");
	usleep(10000000);

	printf("do_stuff: done\n");
	return NULL;
}

int main()
{
	pthread_t thread;
	int rc;

	if ((rc = pthread_create(&thread, NULL, do_stuff, NULL)) != 0) {
		errno = rc;
		perror("pthread_create");
		return -1;
	}

	if ((rc = pthread_join(thread, NULL)) != 0) {
		errno = rc;
		perror("pthread_join");
		return -1;
	}

	return 0;
}
