IDIR = ../include
ODIR = ../obj
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic

.PHONY: all clean

all: test1 test2 test3 test4 valgrind_pthreads_osx_bug

OBJECTS = $(wildcard $(ODIR)/*.o)

%: %.c $(OBJECTS)
	$(CC) $(CFLAGS) -I$(IDIR) $(OBJECTS) $@.c $(LIBS) -o $@

clean:
	rm -f test1
	rm -f test2
	rm -f test3
	rm -f test4
	rm -f valgrind_pthreads_osx_bug
