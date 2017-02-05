TARGET = libasyncio.dylib
IDIR = include
SRCDIR = src
ODIR = obj
LIBS = -lpthread
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -fvisibility=hidden
LD = ld
LDFLAGS = -dylib -macosx_version_min 10.8

.PHONY: default all objdir tests clean

default: $(TARGET)
all: default
objdir:
	mkdir obj

HEADERS = $(wildcard $(IDIR)/*.h)
SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(patsubst $(SRCDIR)/%.c, $(ODIR)/%.o, $(SOURCES))

$(ODIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	$(CC) -c $(CFLAGS) -I$(IDIR) $< -o $@

$(TARGET): objdir $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@

tests:
	$(MAKE) -C tests

clean:
	rm -f $(TARGET)
	rm -rf obj
	$(MAKE) -C tests clean
