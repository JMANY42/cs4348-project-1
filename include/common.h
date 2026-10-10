/*
 * common.h - names, limits and message/shared-memory formats shared by
 * all four processes (client, dispatcher, worker, logger).
 */
#ifndef COMMON_H
#define COMMON_H

#include <limits.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* IPC object names */
#define REQ_FIFO      "/tmp/jobsys_req"        /* client -> dispatcher (shared by all clients) */
#define RESP_FIFO_FMT "/tmp/jobsys_resp_%d"    /* dispatcher -> client (one per client pid)    */
#define MQ_NAME       "/jobsys_mq"             /* dispatcher -> worker                         */
#define SHM_NAME      "/jobsys_shm"            /* worker -> dispatcher, worker -> logger       */
#define LOG_FILE      "jobs.log"

/* Limits */
#define MAX_ARGS       64
#define DESC_LEN       128
#define SUMMARY_LEN    256
#define MAX_DATA_BYTES (1 << 20)                       /* 1 MB output cap (M1 assumption) */
#define MAX_DATA_ITEMS (MAX_DATA_BYTES / sizeof(int64_t))
#define MAX_FIB_N      92                              /* fib(92) is the largest that fits in int64 */

typedef enum { JOB_PING, JOB_SORT, JOB_PRIMES_RANGE, JOB_FIB, JOB_IS_PRIME, JOB_TYPE_COUNT } job_type_t;
typedef enum { ST_QUEUED, ST_RUNNING, ST_DONE, ST_ERROR } job_status_t;

/* Client -> Dispatcher, over REQ_FIFO */
typedef struct {
    job_type_t type;
    pid_t client_pid;
    int nargs;
    int64_t args[MAX_ARGS];
} client_request_t;

/* Dispatcher -> Client, over the client's response FIFO */
typedef struct {
    int job_id;
    job_type_t type;
    job_status_t status;
    char summary[SUMMARY_LEN];
} client_response_t;

/* Dispatcher -> Worker, over the message queue */
typedef struct {
    int job_id;
    job_type_t type;
    pid_t client_pid;
    struct timespec time_received;
    char description[DESC_LEN];
    int nargs;
    int64_t args[MAX_ARGS];
} job_msg_t;

/* One completed job, written by the worker into shared memory */
typedef struct {
    int job_id;
    job_type_t type;
    job_status_t status;
    pid_t client_pid, worker_pid;
    struct timespec time_received, time_completed;
    char description[DESC_LEN];
    char summary[SUMMARY_LEN];
    int logger_done;        /* logger has written this record to the log file   */
    int dispatcher_done;    /* dispatcher has forwarded this record to a client */
    size_t length;          /* number of valid items in data[] (0 in M2)        */
    int64_t data[MAX_DATA_ITEMS];
} job_record_t;

/* Layout of the whole shared memory region */
typedef struct {
    sem_t lock;             /* process-shared mutex guarding slot */
    pid_t dispatcher_pid;   /* each process publishes its pid so others can signal it */
    pid_t worker_pid;
    pid_t logger_pid;
    job_record_t slot;      /* single slot: system handles one job at a time */
} shm_region_t;

/*
 * Writes of PIPE_BUF bytes or fewer to a FIFO are atomic, so requests from
 * several clients sharing REQ_FIFO can never interleave.
 */
_Static_assert(sizeof(client_request_t) <= PIPE_BUF, "request must fit in one atomic FIFO write");
_Static_assert(sizeof(client_response_t) <= PIPE_BUF, "response must fit in one atomic FIFO write");

#endif
