IDIR = ../include
ROOTDIR = ../../
CC = gcc
CFLAGS = -Wall -Wextra -Werror -Wmissing-prototypes -pedantic -std=c89

LIBDIR = $(ROOTDIR)

.PHONY: all clean

all: test1 test2 test3 test4 valgrind_pthreads_osx_bug

%: %.c
	$(CC) $(CFLAGS) $@.c -I$(IDIR) -L$(LIBDIR) $(LIBS) -o $@

clean:
	rm -f test1
	rm -f test2
	rm -f test3
	rm -f test4
	rm -f valgrind_pthreads_osx_bug
