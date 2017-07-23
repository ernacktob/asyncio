IDIR = ../include
ROOTDIR = ../../
CC = gcc
CFLAGS = -Wall -Wextra -Werror -Wmissing-prototypes -pedantic -std=c89 -D_POSIX_SOURCE

LIBDIR = $(ROOTDIR)

.PHONY: all clean

all: test1 test2

%: %.c
	$(CC) $(CFLAGS) $@.c -I$(IDIR) -L$(LIBDIR) $(LIBS) -o $@

clean:
	rm -f test1
	rm -f test2
