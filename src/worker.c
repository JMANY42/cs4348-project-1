/*
 * worker.c - the program that actually does the jobs.
 *
 * Takes jobs from the queue, writes each result into shared memory, then
 * signals the dispatcher and the logger that a result is ready.
 * In milestone 2 the jobs aren't really computed yet; it just sends back
 * a placeholder answer.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ipc.h"

static volatile sig_atomic_t stop = 0;

static void on_stop(int sig) { (void)sig; stop = 1; }

// Do the job and store the answer and status in r
// The real work will be added in milestone 3
static void execute_job(const job_msg_t *job, job_record_t *r)
{
    r->length = 0;
    switch (job->type) {
    case JOB_PING:
        snprintf(r->summary, sizeof r->summary, "pong");
        break;
    case JOB_SORT:
    case JOB_PRIMES_RANGE:
    case JOB_FIB:
    case JOB_IS_PRIME:
        snprintf(r->summary, sizeof r->summary, "stub: received \"%s\"", job->description);
        break;
    default:
        snprintf(r->summary, sizeof r->summary, "unknown job type %d", (int)job->type);
        r->status = ST_ERROR;
        return;
    }
    r->status = ST_DONE;
}

/*
 * Shared memory only has room for one job, so don't overwrite it until the
 * logger has saved the last one. If the logger isn't running, don't wait;
 * the logger will check for missed jobs when it starts back up.
 */
static void wait_for_logger(shm_region_t *shm)
{
    struct timespec poll = { 0, 1000000L };   // check every 1 ms
    for (;;) {
        shm_lock(shm);
        int pending = shm->slot.job_id > 0 && !shm->slot.logger_done;
        shm_unlock(shm);
        pid_t logger = shm->logger_pid;
        if (!pending || logger <= 0 || kill(logger, 0) == -1 || stop)
            return;
        nanosleep(&poll, NULL);
    }
}

static void notify(const char *who, pid_t pid)
{
    if (pid <= 0) {
        printf("[worker] %s not registered in shm, skipping signal\n", who);
        return;
    }
    if (kill(pid, SIGUSR1) == -1)
        fprintf(stderr, "[worker] kill(%s %d, SIGUSR1): %s\n", who, (int)pid, strerror(errno));
    else
        printf("[worker] SIGUSR1 -> %s %d\n", who, (int)pid);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_handler(SIGINT, on_stop);
    install_handler(SIGTERM, on_stop);

    mqd_t mq = mq_attach();
    shm_region_t *shm = shm_attach();
    shm->worker_pid = getpid();
    printf("[worker] pid %d attached to %s and %s\n", (int)getpid(), MQ_NAME, SHM_NAME);

    while (!stop) {
        job_msg_t job;
        ssize_t n = mq_receive(mq, (char *)&job, sizeof job, NULL);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("[worker] mq_receive");
            break;
        }
        if (n != (ssize_t)sizeof job) {
            fprintf(stderr, "[worker] unexpected message size %zd, ignored\n", n);
            continue;
        }
        printf("[worker] MQ <- job %d: %s\n", job.job_id, job.description);

        wait_for_logger(shm);
        shm_lock(shm);
        job_record_t *r = &shm->slot;
        r->job_id = job.job_id;
        r->type = job.type;
        r->status = ST_RUNNING;
        r->client_pid = job.client_pid;
        r->worker_pid = getpid();
        r->time_received = job.time_received;
        memcpy(r->description, job.description, sizeof r->description);
        execute_job(&job, r);
        r->time_completed = now_timespec();
        r->logger_done = 0;
        r->dispatcher_done = 0;
        shm_unlock(shm);
        printf("[worker] SHM <- job %d result written\n", job.job_id);

        notify("dispatcher", shm->dispatcher_pid);
        notify("logger", shm->logger_pid);
    }

    printf("[worker] shutting down\n");
    shm->worker_pid = 0;
    munmap(shm, sizeof *shm);
    mq_close(mq);
    return 0;
}
