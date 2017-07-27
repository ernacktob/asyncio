IDIR = ../include
LIBS += -lasyncio
CC = gcc
CFLAGS = -Wall -Wextra -Werror -Wmissing-prototypes -pedantic -std=c89 -DMALLOC_IS_THREAD_SAFE

.PHONY: all incdir libfile header_files clean

all: buffevents_tests fdevents_tests hashtable_tests threadpool_tests

incdir:
	mkdir -p include

HEADERS = $(wildcard $(IDIR)/*.h)

header_files: incdir $(HEADERS)
	cp $(IDIR)/*.h include/

buffevents_tests: libfile header_files
	$(MAKE) -C buffevents_tests

fdevents_tests: libfile header_files
	$(MAKE) -C fdevents_tests

hashtable_tests: libfile header_files
	$(MAKE) -C hashtable_tests

threadpool_tests: libfile header_files
	$(MAKE) -C threadpool_tests

clean:
	rm -rf include
	$(MAKE) -C buffevents_tests clean
	$(MAKE) -C fdevents_tests clean
	$(MAKE) -C hashtable_tests clean
	$(MAKE) -C threadpool_tests clean
