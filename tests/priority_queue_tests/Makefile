IDIR = ../include
ODIR = ../obj
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic

.PHONY: all clean

all: test1

OBJECTS = $(wildcard $(ODIR)/*.o)

%: %.c $(OBJECTS)
	$(CC) $(CFLAGS) -I$(IDIR) $(OBJECTS) $@.c $(LIBS) -o $@

clean:
	rm -f test1
