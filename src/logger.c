/*
 * logger.c - persistent record of completed jobs.
 *
 * Sleeps until the worker sends SIGUSR1, then reads the completed job from
 * shared memory and appends one JSON line to LOG_FILE. Records it has not
 * yet logged (logger_done == 0) are picked up again on restart.
 */
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ipc.h"

static volatile sig_atomic_t job_ready = 0;
static volatile sig_atomic_t stop = 0;

static void on_usr1(int sig) { (void)sig; job_ready = 1; }
static void on_stop(int sig) { (void)sig; stop = 1; }

static FILE *log_fp;
static long jobs_logged = 0;
static long jobs_failed = 0;

/* Write s as a JSON string literal, escaping quotes, backslashes and control characters. */
static void put_json_string(const char *s)
{
    fputc('"', log_fp);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(log_fp, "\\%c", c);
        else if (c < 0x20)
            fprintf(log_fp, "\\u%04x", c);
        else
            fputc(c, log_fp);
    }
    fputc('"', log_fp);
}

static void log_event(const char *event)
{
    char now[40];
    struct timespec ts = now_timespec();
    format_time(&ts, now, sizeof now);
    fprintf(log_fp, "{\"event\":\"%s\",\"time\":\"%s\",\"logger_pid\":%d,"
                    "\"jobs_logged\":%ld,\"jobs_failed\":%ld}\n",
            event, now, (int)getpid(), jobs_logged, jobs_failed);
    fflush(log_fp);
}

/* Log the shared memory slot if it holds a job not yet written to the log. */
static void log_slot(shm_region_t *shm)
{
    shm_lock(shm);
    job_record_t *r = &shm->slot;
    if (r->job_id > 0 && !r->logger_done) {
        char received[40], completed[40];
        format_time(&r->time_received, received, sizeof received);
        format_time(&r->time_completed, completed, sizeof completed);

        fprintf(log_fp, "{\"event\":\"job_completed\",\"job_id\":%d,\"type\":\"%s\",\"status\":\"%s\","
                        "\"time_received\":\"%s\",\"time_completed\":\"%s\","
                        "\"client_pid\":%d,\"worker_pid\":%d,\"description\":",
                r->job_id, job_type_name(r->type), job_status_name(r->status),
                received, completed, (int)r->client_pid, (int)r->worker_pid);
        put_json_string(r->description);
        fputs(",\"result_summary\":", log_fp);
        put_json_string(r->summary);
        fputs("}\n", log_fp);
        fflush(log_fp);

        r->logger_done = 1;
        if (r->status == ST_ERROR)
            jobs_failed++;
        else
            jobs_logged++;
        printf("[logger] SHM -> job %d written to %s\n", r->job_id, LOG_FILE);
    }
    shm_unlock(shm);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    install_handler(SIGINT, on_stop);
    install_handler(SIGTERM, on_stop);
    install_handler(SIGUSR1, on_usr1);

    /* Same pattern as the dispatcher: SIGUSR1 is only delivered inside sigsuspend(). */
    sigset_t usr1, waitmask;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &usr1, &waitmask) == -1)
        die("sigprocmask");
    sigdelset(&waitmask, SIGUSR1);

    log_fp = fopen(LOG_FILE, "a");
    if (!log_fp)
        die("fopen " LOG_FILE);

    shm_region_t *shm = shm_attach();
    shm->logger_pid = getpid();
    printf("[logger] pid %d attached to %s, logging to %s\n", (int)getpid(), SHM_NAME, LOG_FILE);
    log_event("logger_start");

    log_slot(shm);                        /* recover a job completed while we were down */

    while (!stop) {
        while (!job_ready && !stop)
            sigsuspend(&waitmask);
        if (job_ready) {
            job_ready = 0;
            printf("[logger] SIGUSR1 <- worker\n");
            log_slot(shm);
        }
    }

    printf("[logger] shutting down (%ld jobs logged, %ld failed)\n", jobs_logged, jobs_failed);
    log_event("logger_stop");
    shm->logger_pid = 0;
    munmap(shm, sizeof *shm);
    fclose(log_fp);
    return 0;
}
