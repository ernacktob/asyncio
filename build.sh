#!/bin/sh

mkdir -p obj
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/threadpool.c -o obj/threadpool.o
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/fdevents.c -o obj/fdevents.o
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/cancellations.c -o obj/cancellations.o
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/safe_malloc.c -o obj/safe_malloc.o
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/logging.c -o obj/logging.o
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/hashtable.c -o obj/hashtable.o
