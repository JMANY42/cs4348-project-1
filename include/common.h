/*
 * common.h - settings and data layouts used by all four programs
 * (client, dispatcher, worker, logger), so they agree on how to talk
 * to each other.
 */
#ifndef COMMON_H
#define COMMON_H

#include <limits.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* Names of the pipes, queue, shared memory, and log file the programs share */
#define REQ_FIFO      "/tmp/jobsys_req"        // clients send requests to the dispatcher here (one pipe for everyone)
#define RESP_FIFO_FMT "/tmp/jobsys_resp_%d"    // dispatcher replies to a client here (each client gets its own pipe)
#define MQ_NAME       "/jobsys_mq"             // dispatcher hands jobs to the worker here
#define SHM_NAME      "/jobsys_shm"            // worker leaves finished results here for the dispatcher and logger
#define LOG_FILE      "jobs.log"

/* Size limits */
#define MAX_ARGS       64
#define DESC_LEN       128
#define SUMMARY_LEN    256
#define MAX_DATA_BYTES (1 << 20)                       // a job's output can be at most 1 MB (assumed in milestone 1)
#define MAX_DATA_ITEMS (MAX_DATA_BYTES / sizeof(int64_t))
#define MAX_FIB_N      92                              // fib(93) and up are too big for a 64-bit integer

typedef enum { JOB_PING, JOB_SORT, JOB_PRIMES_RANGE, JOB_FIB, JOB_IS_PRIME, JOB_TYPE_COUNT } job_type_t;
typedef enum { ST_QUEUED, ST_RUNNING, ST_DONE, ST_ERROR } job_status_t;

/* A job request the client sends to the dispatcher */
typedef struct {
    job_type_t type;
    pid_t client_pid;
    int nargs;
    int64_t args[MAX_ARGS];
} client_request_t;

/* The reply the dispatcher sends back to the client */
typedef struct {
    int job_id;
    job_type_t type;
    job_status_t status;
    char summary[SUMMARY_LEN];
} client_response_t;

/* A job the dispatcher passes along to the worker */
typedef struct {
    int job_id;
    job_type_t type;
    pid_t client_pid;
    struct timespec time_received;
    char description[DESC_LEN];
    int nargs;
    int64_t args[MAX_ARGS];
} job_msg_t;

/* A finished job. The worker fills this in for the others to read. */
typedef struct {
    int job_id;
    job_type_t type;
    job_status_t status;
    pid_t client_pid, worker_pid;
    struct timespec time_received, time_completed;
    char description[DESC_LEN];
    char summary[SUMMARY_LEN];
    int logger_done;        // set once the logger has saved this job to the log file
    int dispatcher_done;    // set once the dispatcher has sent the result to the client
    size_t length;          // how many entries in data[] are filled in (always 0 in milestone 2)
    int64_t data[MAX_DATA_ITEMS];
} job_record_t;

/* Everything stored in shared memory */
typedef struct {
    sem_t lock;             // lock: only one program may touch slot at a time
    pid_t dispatcher_pid;   // each program's process ID, so the others can send it signals
    pid_t worker_pid;
    pid_t logger_pid;
    job_record_t slot;      // room for just one job, since the system runs one job at a time
} shm_region_t;

/*
 * The OS guarantees that a small enough write to a pipe arrives all in one
 * piece. Keeping requests and replies under that size means two clients
 * writing at once can't get their messages mixed together.
 */
_Static_assert(sizeof(client_request_t) <= PIPE_BUF, "request must fit in one atomic FIFO write");
_Static_assert(sizeof(client_response_t) <= PIPE_BUF, "response must fit in one atomic FIFO write");

#endif
