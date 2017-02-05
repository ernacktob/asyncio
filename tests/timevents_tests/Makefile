IDIR = ../include
ODIR = ../obj
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic
LIBS += -lm

.PHONY: all clean

all: test1 test2 test3

%:
	$(CC) $(CFLAGS) -I$(IDIR) $(ODIR)/*.o $@.c $(LIBS) -o $@

clean:
	rm -f test1
	rm -f test2
	rm -f test3
