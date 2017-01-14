#!/bin/sh

mkdir -p obj
gcc -g -DDEBUG -c -Wall -Wextra -pedantic -Iinclude src/threadpool.c -o obj/threadpool.o
gcc -g -DDEBUG -c -Wall -Wextra -pedantic -Iinclude src/safe_malloc.c -o obj/safe_malloc.o
gcc -g -DDEBUG -c -Wall -Wextra -pedantic -Iinclude src/logging.c -o obj/logging.o
