/*
 * ipc.h - helpers for creating/attaching IPC objects plus small utilities
 * used by more than one process.
 */
#ifndef IPC_H
#define IPC_H

#include <mqueue.h>
#include "common.h"

/* Print msg with errno text and exit(1). */
void die(const char *msg);

/* Install fn for sig with sigaction; no SA_RESTART so blocking calls return EINTR. */
void install_handler(int sig, void (*fn)(int));

/* Dispatcher creates fresh objects; worker/logger attach, retrying until they exist. */
shm_region_t *shm_create(void);
shm_region_t *shm_attach(void);
mqd_t mq_create(void);
mqd_t mq_attach(void);

/* Process-shared semaphore around the shared memory slot (retries on EINTR). */
void shm_lock(shm_region_t *shm);
void shm_unlock(shm_region_t *shm);

struct timespec now_timespec(void);
void format_time(const struct timespec *ts, char *buf, size_t n);

const char *job_type_name(job_type_t type);
const char *job_status_name(job_status_t status);
/* Whether nargs is a legal argument count for type. */
int job_args_ok(job_type_t type, int nargs);
void describe_job(job_type_t type, int nargs, const int64_t *args, char *buf, size_t n);

#endif
