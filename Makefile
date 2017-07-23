CFLAGS = -Wall -Wextra -Werror -Wmissing-prototypes -pedantic -std=c89 -fPIC -DMALLOC_IS_THREAD_SAFE
UNAME = $(shell uname)

ifeq ($(UNAME), Darwin)
TARGET = libasyncio.dylib
LDFLAGS = -dylib -macosx_version_min 10.8 -install_name $(shell pwd)/$(TARGET)
LIBS = -lpthread
else
LDFLAGS = -shared
TARGET = libasyncio.so
CFLAGS += -D_GNU_SOURCE
EXAMPLES_CFLAGS = -Wl,-rpath=$(shell pwd)	# Tell linker where to look for libasyncio.so when linking examples
LIBS = -lpthread -lrt
endif

export EXAMPLES_CFLAGS	# Make available for sub-makes
export LIBS

IDIR = include
SRCDIR = src
ODIR = obj
CC = gcc
LD = ld

.PHONY: default all objdir public_headers tests examples clean

default: $(TARGET)
all: default
objdir:
	mkdir -p obj
public_headers:
	cp $(IDIR)/asyncio_*.h .

HEADERS = $(wildcard $(IDIR)/*.h)
SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(patsubst $(SRCDIR)/%.c, $(ODIR)/%.o, $(SOURCES))

$(ODIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	$(CC) -c $(CFLAGS) -I$(IDIR) $< -o $@

$(TARGET): objdir public_headers $(OBJECTS)
	$(LD) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@

tests: $(TARGET)
	$(MAKE) -C tests

examples: $(TARGET)
	$(MAKE) -C examples

clean:
	rm -f $(TARGET)
	rm -f asyncio_*.h
	rm -rf obj
	$(MAKE) -C tests clean
	$(MAKE) -C examples clean
