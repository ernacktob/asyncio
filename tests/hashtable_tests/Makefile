IDIR = ../include
ODIR = ../obj
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic

.PHONY: all clean

all: test1

%:
	$(CC) $(CFLAGS) -I$(IDIR) $(ODIR)/*.o $@.c $(LIBS) -o $@

clean:
	rm -f test1
