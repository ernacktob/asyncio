UNAME = $(shell uname)

ifeq ($(UNAME), Darwin)
LDFLAGS = -dylib -macosx_version_min 10.8
TARGET = libasyncio.dylib
LIBS = -lpthread
else
LDFLAGS = -shared
TARGET = libasyncio.so
LIBS = -lpthread -lrt
endif

export LIBS

IDIR = include
SRCDIR = src
ODIR = obj
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -fPIC -fvisibility=hidden
LD = ld

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
