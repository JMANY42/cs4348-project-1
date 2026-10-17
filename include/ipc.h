// ipc.h - helper functions for setting up the shared queue and shared
// memory, plus small utilities that more than one program uses.
#ifndef IPC_H
#define IPC_H

#include <mqueue.h>
#include "common.h"

// Print msg along with the system's error message, then quit.
void die(const char *msg);

// Run fn whenever signal sig arrives. A signal also wakes up any call that is waiting.
void install_handler(int sig, void (*fn)(int));

// The dispatcher creates these. The worker and logger connect to them, waiting until they exist.
shm_region_t *shm_create(void);
shm_region_t *shm_attach(void);
mqd_t mq_create(void);
mqd_t mq_attach(void);

// Take and release the lock on shared memory, so only one program uses it at a time.
void shm_lock(shm_region_t *shm);
void shm_unlock(shm_region_t *shm);

// Add a system event to shared memory for the logger and wake the logger up.
// job_id is 0 for events that aren't about a job. detail is printf-style.
void post_event(shm_region_t *shm, const char *source, const char *name, int job_id,
                const char *fmt, ...) __attribute__((format(printf, 5, 6)));

struct timespec now_timespec(void);
void format_time(const struct timespec *ts, char *buf, size_t n);

const char *job_type_name(job_type_t type);
const char *job_status_name(job_status_t status);
// Returns true if a job of this type can take nargs arguments.
int job_args_ok(job_type_t type, int nargs);
// Checks the arguments' values too. Returns 0 if the job is OK, or -1 with the reason in err.
int job_validate(job_type_t type, int nargs, const int64_t *args, char *err, size_t n);
void describe_job(job_type_t type, int nargs, const int64_t *args, char *buf, size_t n);

#endif
