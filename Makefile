CC      = gcc
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -g -pthread -Iinclude
LDLIBS  = -lrt -pthread
BINS    = client dispatcher worker logger
COMMON  = src/ipc.c include/ipc.h include/common.h

.PHONY: all clean clean-ipc run

all: $(BINS)

$(BINS): %: src/%.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $< src/ipc.c $(LDLIBS)

run: all
	./scripts/demo.sh

clean:
	rm -f $(BINS)

# Delete leftover pipes, queue and shared memory if a program crashed
clean-ipc:
	rm -f /tmp/jobsys_req /tmp/jobsys_resp_* /dev/shm/jobsys_shm /dev/mqueue/jobsys_mq
