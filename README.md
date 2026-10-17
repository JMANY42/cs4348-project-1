# CS 4348 PROJECT 1 MILESTONE 3 SUBMISSION
Valerie Nelson and David Lewis

## Status report

### Completed
* **Dispatching:** clients send requests over a shared named pipe. The
  dispatcher validates each one, gives it a job ID, and queues it for the
  worker on a POSIX message queue. Replies go back on each client's own pipe.
* **Job execution:** the worker computes `ping`, `sort` (up to 64 integers),
  `primes lo hi` (segmented sieve), `fib n` (n <= 92), and `isprime n`
  (Miller-Rabin, any 64-bit integer).
* **Results through shared memory:** the worker writes the status, summary,
  and up to 1 MB of result values into shared memory, then signals the
  dispatcher and logger with `SIGUSR1`. The dispatcher reads the result and
  sends the client the summary plus the first 64 values.
* **Event logging:** the dispatcher and worker post system events (start/stop,
  job received/queued/started/rejected, response sent/failed) to a ring buffer
  in shared memory. The logger writes them, plus a full record of each
  completed job, to `jobs.log` as JSON lines. Events from while the logger
  was stopped are written once it restarts.
* **Multiple requests:** clients can send many jobs, and several clients can
  run at once. The demo runs 19 jobs across one and three concurrent clients,
  all completed and logged, with clean IPC teardown on shutdown.

### Remaining (milestone 4)
* Crash handling: detect a dead worker and add job timeouts. Right now the
  dispatcher waits forever if the worker dies mid-job.
* Shared memory holds one result, so jobs that finish while the logger is
  down can be overwritten before they are logged.
* Jobs run one at a time. Plan: multiple workers, each with its own result slot.
* Clients only receive the first 64 values of a large result.

## To Run
```
make            # build all four binaries
make run        # scripted demo
make clean-ipc  # remove leftover IPC objects after a crash
```
Or start each program in its own terminal, dispatcher first:
```
./dispatcher
./logger
./worker
./client        # type 'help' for commands
```
