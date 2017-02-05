TARGET = echo_server
ROOTDIR = ../../
IDIR = $(ROOTDIR)
LIBDIR = $(ROOTDIR)
LIBS = -lasyncio

CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic

.PHONY: default all clean

default: $(TARGET)
all: default

SOURCES = $(wildcard *.c)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -I$(IDIR) -L$(LIBDIR) $(SOURCES) $(LIBS) -o $(TARGET)

clean:
	rm -f $(TARGET)
