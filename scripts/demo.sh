#!/usr/bin/env bash
# Milestone 2 demo: start dispatcher, logger and worker, push jobs through
# every IPC path, then shut down and show that all IPC objects are removed.
set -u
cd "$(dirname "$0")/.."

make -s all || exit 1
make -s clean-ipc
rm -f jobs.log

section() { printf '\n===== %s =====\n' "$1"; }

section "starting processes"
./dispatcher & DPID=$!
sleep 0.3
./logger & LPID=$!
./worker & WPID=$!
sleep 0.5

section "IPC objects"
ls -l /tmp/jobsys_req /dev/shm/jobsys_shm /dev/mqueue/jobsys_mq
echo "queue state: $(cat /dev/mqueue/jobsys_mq)"

section "single client"
printf 'ping\nisprime 7\nfib 10\nsort 5 3 1\nprimes 1 100\nfib 1000\nbogus\nquit\n' | ./client

section "two concurrent clients"
(printf 'ping\nfib 5\nquit\n' | ./client) & C1=$!
(printf 'isprime 11\nsort 9 8\nquit\n' | ./client) & C2=$!
wait "$C1" "$C2"

section "logger recovery: job completes while logger is down"
kill -INT "$LPID"; wait "$LPID"
printf 'isprime 13\nquit\n' | ./client
./logger & LPID=$!
sleep 0.5

section "shutdown"
kill -INT "$WPID" "$LPID"
wait "$WPID" "$LPID"
kill -INT "$DPID"
wait "$DPID"

section "jobs.log"
cat jobs.log

section "leftover IPC objects (expect none)"
ls /tmp/jobsys_* /dev/shm/jobsys_shm /dev/mqueue/jobsys_mq 2>/dev/null || echo "none - clean shutdown"
