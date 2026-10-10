#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ATTACH_RETRIES 50                 // try 50 times, 100 ms apart: wait up to 5 seconds for the dispatcher

void die(const char *msg)
{
    perror(msg);
    exit(1);
}

void install_handler(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                      // a signal interrupts a waiting read so the loop can check for shutdown
    if (sigaction(sig, &sa, NULL) == -1)
        die("sigaction");
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

shm_region_t *shm_create(void)
{
    shm_unlink(SHM_NAME);                 // remove any leftover copy from a crash
    int fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd == -1)
        die("shm_open (create)");
    if (ftruncate(fd, sizeof(shm_region_t)) == -1)
        die("ftruncate");
    shm_region_t *shm = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED)
        die("mmap");
    close(fd);                            // the memory stays usable after closing the file

    memset(shm, 0, sizeof *shm);
    if (sem_init(&shm->lock, 1, 1) == -1) // the 1 lets other programs share this lock
        die("sem_init");
    return shm;
}

shm_region_t *shm_attach(void)
{
    int fd = -1;
    for (int i = 0; i < ATTACH_RETRIES && fd == -1; i++) {
        fd = shm_open(SHM_NAME, O_RDWR, 0);
        if (fd == -1) {
            if (errno != ENOENT)
                die("shm_open (attach)");
            sleep_ms(100);
        }
    }
    if (fd == -1) {
        fprintf(stderr, "shared memory %s not found - is the dispatcher running?\n", SHM_NAME);
        exit(1);
    }
    /* The dispatcher may not have finished sizing the memory yet, so wait until it has. */
    struct stat st;
    for (int i = 0; i < ATTACH_RETRIES; i++) {
        if (fstat(fd, &st) == -1)
            die("fstat");
        if ((size_t)st.st_size >= sizeof(shm_region_t))
            break;
        sleep_ms(100);
    }
    shm_region_t *shm = mmap(NULL, sizeof(shm_region_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED)
        die("mmap");
    close(fd);
    return shm;
}

mqd_t mq_create(void)
{
    struct mq_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(job_msg_t);

    mq_unlink(MQ_NAME);                   // start a new queue so our size settings take effect
    mqd_t mq = mq_open(MQ_NAME, O_CREAT | O_EXCL | O_WRONLY, 0666, &attr);
    if (mq == (mqd_t)-1)
        die("mq_open (create)");
    return mq;
}

mqd_t mq_attach(void)
{
    for (int i = 0; i < ATTACH_RETRIES; i++) {
        mqd_t mq = mq_open(MQ_NAME, O_RDONLY);
        if (mq != (mqd_t)-1)
            return mq;
        if (errno != ENOENT)
            die("mq_open (attach)");
        sleep_ms(100);
    }
    fprintf(stderr, "message queue %s not found - is the dispatcher running?\n", MQ_NAME);
    exit(1);
}

void shm_lock(shm_region_t *shm)
{
    while (sem_wait(&shm->lock) == -1) {
        if (errno != EINTR)
            die("sem_wait");
    }
}

void shm_unlock(shm_region_t *shm)
{
    if (sem_post(&shm->lock) == -1)
        die("sem_post");
}

struct timespec now_timespec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts;
}

void format_time(const struct timespec *ts, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&ts->tv_sec, &tm);
    size_t len = strftime(buf, n, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(buf + len, n - len, ".%03ld", ts->tv_nsec / 1000000L);
}

const char *job_type_name(job_type_t type)
{
    static const char *names[] = { "ping", "sort", "primes", "fib", "isprime" };
    return (type >= 0 && type < JOB_TYPE_COUNT) ? names[type] : "unknown";
}

const char *job_status_name(job_status_t status)
{
    static const char *names[] = { "queued", "running", "done", "error" };
    return (status >= ST_QUEUED && status <= ST_ERROR) ? names[status] : "unknown";
}

int job_args_ok(job_type_t type, int nargs)
{
    switch (type) {
    case JOB_PING:         return nargs == 0;
    case JOB_SORT:         return nargs >= 1 && nargs <= MAX_ARGS;
    case JOB_PRIMES_RANGE: return nargs == 2;
    case JOB_FIB:          return nargs == 1;
    case JOB_IS_PRIME:     return nargs == 1;
    default:               return 0;
    }
}

void describe_job(job_type_t type, int nargs, const int64_t *args, char *buf, size_t n)
{
    switch (type) {
    case JOB_PING:
        snprintf(buf, n, "ping");
        break;
    case JOB_SORT:
        snprintf(buf, n, "sort %d numbers", nargs);
        break;
    case JOB_PRIMES_RANGE:
        snprintf(buf, n, "primes in [%lld, %lld]", (long long)args[0], (long long)args[1]);
        break;
    case JOB_FIB:
        snprintf(buf, n, "first %lld Fibonacci numbers", (long long)args[0]);
        break;
    case JOB_IS_PRIME:
        snprintf(buf, n, "is %lld prime?", (long long)args[0]);
        break;
    default:
        snprintf(buf, n, "unknown job type %d", (int)type);
        break;
    }
}
