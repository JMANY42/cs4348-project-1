#!/usr/bin/env bash
# Milestone 3 demo: start the dispatcher, logger and worker, run real jobs
# from one client and from several clients at once, show that results come
# back through shared memory and that system events are logged, then shut
# down and show that all the shared pipes, queue and memory were cleaned up.
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

section "single client: every job type"
printf '%s\n' 'ping' 'isprime 7' 'isprime 1000000007' 'isprime 1000000008' \
    'fib 20' 'sort 42 -7 19 3 0 88 -15' 'primes 1 100' \
    'primes 999999000000 1000000000000' 'quit' | ./client

section "single client: bad input is rejected"
printf '%s\n' 'fib 1000' 'primes 50 10' 'primes 1 2000000000000' 'isprime' 'bogus' 'quit' | ./client

section "large result: primes in [1, 10000000] (only the first values are sent to the client)"
printf 'primes 1 10000000\nquit\n' | ./client

section "three concurrent clients"
(printf 'ping\nfib 5\nisprime 97\nquit\n' | ./client) & C1=$!
(printf 'isprime 11\nsort 9 8 7\nprimes 10 30\nquit\n' | ./client) & C2=$!
(printf 'fib 10\nsort 5 1 4\nping\nquit\n' | ./client) & C3=$!
wait "$C1" "$C2" "$C3"

section "logger recovery: job completes while logger is down"
kill -INT "$LPID"; wait "$LPID"
printf 'isprime 13\nquit\n' | ./client
./logger & LPID=$!
sleep 0.5

section "shutdown"
kill -INT "$WPID"; wait "$WPID"
kill -INT "$DPID"; wait "$DPID"
sleep 0.3                                  # let the logger record the shutdown events
kill -INT "$LPID"; wait "$LPID"

section "jobs.log"
cat jobs.log

section "log summary"
echo "jobs completed: $(grep -c '"event":"job_completed"' jobs.log)"
echo "system events:  $(grep -vc '"event":"job_completed"' jobs.log)"

section "leftover IPC objects (expect none)"
ls /tmp/jobsys_* /dev/shm/jobsys_shm /dev/mqueue/jobsys_mq 2>/dev/null || echo "none - clean shutdown"
