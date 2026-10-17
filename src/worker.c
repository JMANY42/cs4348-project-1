// worker.c - the program that actually does the jobs.
//
// Takes jobs from the queue, works out each answer, writes the result
// (a short summary plus the full list of values) into shared memory, then
// signals the dispatcher and the logger that a result is ready.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ipc.h"

static volatile sig_atomic_t stop = 0;

static void on_stop(int sig) { (void)sig; stop = 1; }

// The answer is built here first, so the shared memory lock is only held while copying it in
static int64_t result[MAX_DATA_ITEMS];

static int cmp_int64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t mulmod(uint64_t a, uint64_t b, uint64_t m)
{
    return (uint64_t)((unsigned __int128)a * b % m);
}

static uint64_t powmod(uint64_t base, uint64_t exp, uint64_t m)
{
    uint64_t r = 1;
    base %= m;
    while (exp > 0) {
        if (exp & 1)
            r = mulmod(r, base, m);
        base = mulmod(base, base, m);
        exp >>= 1;
    }
    return r;
}

// Miller-Rabin test. Checking these 12 bases gives the right answer for every 64-bit number.
static int is_prime(int64_t value)
{
    if (value < 2)
        return 0;
    uint64_t n = (uint64_t)value;
    static const uint64_t bases[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37 };
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        if (n % bases[i] == 0)
            return n == bases[i];
    }
    uint64_t d = n - 1;
    int s = 0;
    while ((d & 1) == 0) {
        d >>= 1;
        s++;
    }
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        uint64_t x = powmod(bases[i], d, n);
        if (x == 1 || x == n - 1)
            continue;
        int composite = 1;
        for (int r = 1; r < s && composite; r++) {
            x = mulmod(x, x, n);
            if (x == n - 1)
                composite = 0;
        }
        if (composite)
            return 0;
    }
    return 1;
}

// Segmented sieve: find primes up to sqrt(hi) first, then use them to cross
// out composites in [lo, hi]. Stores at most cap primes in out but counts all
// of them in *total. Returns how many were stored, or -1 if out of memory.
static long primes_range(int64_t lo, int64_t hi, int64_t *out, size_t cap, size_t *total)
{
    *total = 0;
    if (lo < 2)
        lo = 2;
    if (hi < lo)
        return 0;

    int64_t root = 0;
    while ((root + 1) * (root + 1) <= hi)
        root++;
    char *small = calloc((size_t)root + 1, 1);       // small[i] = 1 means i is not prime
    char *seg = calloc((size_t)(hi - lo + 1), 1);    // seg[i] = 1 means lo + i is not prime
    if (!small || !seg) {
        free(small);
        free(seg);
        return -1;
    }

    for (int64_t p = 2; p <= root; p++) {
        if (small[p])
            continue;
        for (int64_t m = p * p; m <= root; m += p)
            small[m] = 1;
        int64_t start = (lo + p - 1) / p * p;
        if (start < p * p)
            start = p * p;
        for (int64_t m = start; m <= hi; m += p)
            seg[m - lo] = 1;
    }

    size_t stored = 0;
    for (int64_t i = 0; i <= hi - lo; i++) {
        if (seg[i])
            continue;
        if (stored < cap)
            out[stored++] = lo + i;
        (*total)++;
    }
    free(small);
    free(seg);
    return (long)stored;
}

// Do the job: put the values in result[], the count in *length, and a
// one-line summary in summary. Returns the job's final status.
static job_status_t execute_job(const job_msg_t *job, size_t *length, char *summary, size_t n)
{
    char err[SUMMARY_LEN];
    *length = 0;
    if (job_validate(job->type, job->nargs, job->args, err, sizeof err) == -1) {
        snprintf(summary, n, "%s", err);
        return ST_ERROR;
    }

    switch (job->type) {
    case JOB_PING:
        snprintf(summary, n, "pong");
        break;

    case JOB_SORT:
        memcpy(result, job->args, job->nargs * sizeof(int64_t));
        qsort(result, job->nargs, sizeof(int64_t), cmp_int64);
        *length = job->nargs;
        snprintf(summary, n, "sorted %d numbers (min %lld, max %lld)", job->nargs,
                 (long long)result[0], (long long)result[job->nargs - 1]);
        break;

    case JOB_PRIMES_RANGE: {
        size_t total;
        long stored = primes_range(job->args[0], job->args[1], result, MAX_DATA_ITEMS, &total);
        if (stored < 0) {
            snprintf(summary, n, "out of memory");
            return ST_ERROR;
        }
        *length = (size_t)stored;
        if ((size_t)stored < total)
            snprintf(summary, n, "%zu primes in [%lld, %lld] (first %ld kept)", total,
                     (long long)job->args[0], (long long)job->args[1], stored);
        else
            snprintf(summary, n, "%zu primes in [%lld, %lld]", total,
                     (long long)job->args[0], (long long)job->args[1]);
        break;
    }

    case JOB_FIB: {
        int64_t count = job->args[0];
        for (int64_t i = 0; i < count; i++)
            result[i] = i < 2 ? i : result[i - 1] + result[i - 2];
        *length = (size_t)count;
        if (count == 0)
            snprintf(summary, n, "no Fibonacci numbers requested");
        else
            snprintf(summary, n, "first %lld Fibonacci numbers (last = %lld)",
                     (long long)count, (long long)result[count - 1]);
        break;
    }

    case JOB_IS_PRIME: {
        int prime = is_prime(job->args[0]);
        result[0] = prime;
        *length = 1;
        snprintf(summary, n, "%lld is %s", (long long)job->args[0], prime ? "prime" : "not prime");
        break;
    }

    default:
        snprintf(summary, n, "unknown job type %d", (int)job->type);
        return ST_ERROR;
    }
    return ST_DONE;
}

// Shared memory only has room for one job, so don't overwrite it until the
// logger has saved the last one. If the logger isn't running, don't wait;
// the logger will check for missed jobs when it starts back up.
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
    post_event(shm, "worker", "worker_start", 0, "attached to %s and %s", MQ_NAME, SHM_NAME);

    long jobs_run = 0;
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
        post_event(shm, "worker", "job_started", job.job_id, "%s", job.description);

        char summary[SUMMARY_LEN];
        size_t length;
        job_status_t status = execute_job(&job, &length, summary, sizeof summary);
        jobs_run++;

        wait_for_logger(shm);
        shm_lock(shm);
        job_record_t *r = &shm->slot;
        r->job_id = job.job_id;
        r->type = job.type;
        r->status = status;
        r->client_pid = job.client_pid;
        r->worker_pid = getpid();
        r->time_received = job.time_received;
        r->time_completed = now_timespec();
        memcpy(r->description, job.description, sizeof r->description);
        memcpy(r->summary, summary, sizeof r->summary);
        memcpy(r->data, result, length * sizeof(int64_t));
        r->length = length;
        r->logger_done = 0;
        r->dispatcher_done = 0;
        shm_unlock(shm);
        printf("[worker] SHM <- job %d result written (%s, %zu values): %s\n",
               job.job_id, job_status_name(status), length, summary);

        notify("dispatcher", shm->dispatcher_pid);
        notify("logger", shm->logger_pid);
    }

    printf("[worker] shutting down (%ld jobs run)\n", jobs_run);
    post_event(shm, "worker", "worker_stop", 0, "%ld jobs run", jobs_run);
    shm->worker_pid = 0;
    munmap(shm, sizeof *shm);
    mq_close(mq);
    return 0;
}
