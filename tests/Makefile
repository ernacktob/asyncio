IDIR = ../include
ODIR = obj
CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -DMALLOC_IS_THREAD_SAFE

.PHONY: all objdir incdir object_files header_files clean

all: fdevents_tests hashtable_tests priority_queue_tests threadpool_tests timevents_tests
objdir:
	mkdir -p obj
incdir:
	mkdir -p include

HEADERS = $(wildcard $(IDIR)/*.h)
OBJECTS = $(patsubst ../src/%.c, $(ODIR)/%.o, $(wildcard ../src/*.c))

$(ODIR)/%.o: ../src/%.c $(HEADERS)
	$(CC) -c $(CFLAGS) -I$(IDIR) $< -o $@

object_files: objdir $(OBJECTS)
header_files: incdir $(HEADERS)
	cp $(IDIR)/*.h include/

fdevents_tests: object_files header_files
	$(MAKE) -C fdevents_tests

hashtable_tests: object_files header_files
	$(MAKE) -C hashtable_tests

priority_queue_tests: object_files header_files
	$(MAKE) -C priority_queue_tests

threadpool_tests: object_files header_files
	$(MAKE) -C threadpool_tests

timevents_tests: object_files header_files
	$(MAKE) -C timevents_tests

clean:
	rm -rf obj
	rm -rf include
	$(MAKE) -C fdevents_tests clean
	$(MAKE) -C hashtable_tests clean
	$(MAKE) -C priority_queue_tests clean
	$(MAKE) -C threadpool_tests clean
	$(MAKE) -C timevents_tests clean
