IDIR = ../include
ROOTDIR = ../../
CC = gcc
CFLAGS = $(COMMON_CFLAGS) $(USER_CFLAGS)

ifneq ($(UNAME), Darwin)
	CFLAGS += -D_POSIX_SOURCE -D_BSD_SOURCE
endif

LIBDIR = $(ROOTDIR)

.PHONY: all clean

all: test1 test2 test3

%: %.c
	$(CC) $(CFLAGS) $@.c -I$(IDIR) -L$(LIBDIR) $(LIBS) -o $@

clean:
	rm -f test1
	rm -f test2
	rm -f test3
