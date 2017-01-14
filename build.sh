#!/bin/sh

mkdir -p obj
gcc -c -Wall -Wextra -pedantic -Iinclude src/threadpool.c -o obj/threadpool.o
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/safe_malloc.c -o obj/safe_malloc.o
gcc -c -Wall -Wextra -Werror -pedantic -Iinclude src/logging.c -o obj/logging.o
