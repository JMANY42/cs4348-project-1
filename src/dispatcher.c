// dispatcher.c - the middleman that runs the whole system.
//
// Creates the pipe, queue, and shared memory everyone uses. It reads job
// requests from clients, gives each job a number, and passes it to the
// worker through the queue. When the worker signals that it is finished,
// the dispatcher reads the result from shared memory and sends it back to
// the client that asked for it.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ipc.h"

static volatile sig_atomic_t job_done = 0;
static volatile sig_atomic_t stop = 0;

static void on_usr1(int sig) { (void)sig; job_done = 1; }
static void on_stop(int sig) { (void)sig; stop = 1; }

static shm_region_t *shm;

static void send_response(pid_t client_pid, const client_response_t *resp)
{
    char path[64];
    snprintf(path, sizeof path, RESP_FIFO_FMT, (int)client_pid);

    // Don't wait: if the client has quit, give up right away instead of hanging forever.
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "[dispatcher] client %d unreachable (%s), dropping job %d response\n",
                (int)client_pid, strerror(errno), resp->job_id);
        post_event(shm, "dispatcher", "response_failed", resp->job_id,
                   "client %d unreachable: %s", (int)client_pid, strerror(errno));
        return;
    }
    if (write(fd, resp, sizeof *resp) != (ssize_t)sizeof *resp) {
        perror("[dispatcher] write response");
        post_event(shm, "dispatcher", "response_failed", resp->job_id,
                   "write to client %d failed", (int)client_pid);
    } else {
        printf("[dispatcher] FIFO -> job %d response sent to client %d\n", resp->job_id, (int)client_pid);
        post_event(shm, "dispatcher", "response_sent", resp->job_id, "client %d, %s",
                   (int)client_pid, job_status_name(resp->status));
    }
    close(fd);
}

static void reject(const client_request_t *req, const char *why)
{
    client_response_t resp = { .job_id = 0, .type = req->type, .status = ST_ERROR };
    snprintf(resp.summary, sizeof resp.summary, "rejected: %s", why);
    post_event(shm, "dispatcher", "job_rejected", 0, "client %d: %s", (int)req->client_pid, why);
    send_response(req->client_pid, &resp);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);             // a client quitting early shouldn't crash the dispatcher
    install_handler(SIGINT, on_stop);
    install_handler(SIGTERM, on_stop);
    install_handler(SIGUSR1, on_usr1);

    // Hold the worker's "done" signal (SIGUSR1) until we are actually waiting
    // for it. If the worker finishes before we start waiting, the signal is
    // saved for later instead of being lost.
    sigset_t usr1, waitmask;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &usr1, &waitmask) == -1)
        die("sigprocmask");
    sigdelset(&waitmask, SIGUSR1);

    if (mkfifo(REQ_FIFO, 0666) == -1 && errno != EEXIST)
        die("mkfifo " REQ_FIFO);
    mqd_t mq = mq_create();
    shm = shm_create();
    shm->dispatcher_pid = getpid();

    // Open the pipe for writing too, so reads keep waiting for the next client instead of ending when no client is connected.
    int req_fd = open(REQ_FIFO, O_RDWR);
    if (req_fd == -1)
        die("open " REQ_FIFO);

    printf("[dispatcher] pid %d ready: fifo=%s mq=%s shm=%s (%zu bytes)\n",
           (int)getpid(), REQ_FIFO, MQ_NAME, SHM_NAME, sizeof(shm_region_t));
    post_event(shm, "dispatcher", "dispatcher_start", 0, "fifo=%s mq=%s shm=%s",
               REQ_FIFO, MQ_NAME, SHM_NAME);

    int next_job_id = 1;
    long jobs_ok = 0, jobs_failed = 0;
    while (!stop) {
        client_request_t req;
        ssize_t n = read(req_fd, &req, sizeof req);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("[dispatcher] read request");
            break;
        }
        if (n != (ssize_t)sizeof req) {
            fprintf(stderr, "[dispatcher] short request (%zd bytes), ignored\n", n);
            continue;
        }
        char err[SUMMARY_LEN];
        if (job_validate(req.type, req.nargs, req.args, err, sizeof err) == -1) {
            fprintf(stderr, "[dispatcher] bad request from client %d: %s\n", (int)req.client_pid, err);
            reject(&req, err);
            continue;
        }

        job_msg_t job;
        memset(&job, 0, sizeof job);
        job.job_id = next_job_id++;
        job.type = req.type;
        job.client_pid = req.client_pid;
        job.time_received = now_timespec();
        job.nargs = req.nargs;
        memcpy(job.args, req.args, sizeof job.args);
        describe_job(job.type, job.nargs, job.args, job.description, sizeof job.description);
        printf("[dispatcher] FIFO <- client %d: %s -> job %d\n",
               (int)req.client_pid, job.description, job.job_id);
        post_event(shm, "dispatcher", "job_received", job.job_id, "client %d: %s",
                   (int)req.client_pid, job.description);

        job_done = 0;
        // Post before sending, so this event is logged ahead of the worker's job_started
        post_event(shm, "dispatcher", "job_queued", job.job_id, "%s", job.description);
        if (mq_send(mq, (const char *)&job, sizeof job, 0) == -1) {
            perror("[dispatcher] mq_send");
            reject(&req, "could not queue job");
            continue;
        }
        printf("[dispatcher] MQ -> job %d queued for worker\n", job.job_id);

        while (!job_done && !stop)
            sigsuspend(&waitmask);        // sleep until the worker's signal arrives
        if (stop)
            break;
        printf("[dispatcher] SIGUSR1 <- worker: job %d complete\n", job.job_id);

        // Copy the result out of shared memory: the summary, the total
        // number of values, and as many of the values as fit in one reply.
        client_response_t resp;
        memset(&resp, 0, sizeof resp);
        resp.job_id = job.job_id;
        resp.type = job.type;
        shm_lock(shm);
        if (shm->slot.job_id == job.job_id) {
            resp.status = shm->slot.status;
            memcpy(resp.summary, shm->slot.summary, sizeof resp.summary);
            resp.length = shm->slot.length;
            resp.count = resp.length < RESP_DATA_ITEMS ? (int)resp.length : RESP_DATA_ITEMS;
            memcpy(resp.data, shm->slot.data, resp.count * sizeof(int64_t));
            shm->slot.dispatcher_done = 1;
        } else {
            resp.status = ST_ERROR;
            snprintf(resp.summary, sizeof resp.summary, "shared memory holds job %d, expected %d",
                     shm->slot.job_id, job.job_id);
        }
        shm_unlock(shm);
        printf("[dispatcher] SHM -> read job %d result (%s, %zu values)\n",
               resp.job_id, job_status_name(resp.status), resp.length);
        if (resp.status == ST_ERROR)
            jobs_failed++;
        else
            jobs_ok++;

        send_response(job.client_pid, &resp);
    }

    printf("[dispatcher] shutting down (%ld jobs done, %ld failed), removing IPC objects\n",
           jobs_ok, jobs_failed);
    post_event(shm, "dispatcher", "dispatcher_stop", 0, "%ld jobs done, %ld failed", jobs_ok, jobs_failed);
    shm->dispatcher_pid = 0;
    close(req_fd);
    mq_close(mq);
    // The lock isn't destroyed: the logger may still be draining events through its own mapping.
    // The memory itself goes away once every program has unmapped it.
    munmap(shm, sizeof *shm);
    unlink(REQ_FIFO);
    mq_unlink(MQ_NAME);
    shm_unlink(SHM_NAME);
    return 0;
}
