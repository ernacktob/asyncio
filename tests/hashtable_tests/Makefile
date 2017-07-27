IDIR = ../include
ROOTDIR = ../../
CC = gcc
CFLAGS = $(COMMON_CFLAGS) $(USER_CFLAGS)

LIBDIR = $(ROOTDIR)

.PHONY: all clean

all: test1

%: %.c
	$(CC) $(CFLAGS) $@.c -I$(IDIR) -L$(LIBDIR) $(LIBS) -o $@

clean:
	rm -f test1
