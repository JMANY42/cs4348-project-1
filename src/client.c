/*
 * client.c - the only user-facing process.
 *
 * Parses commands, sends a client_request_t over REQ_FIFO and waits for the
 * client_response_t on its own response FIFO. One job at a time.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ipc.h"

static volatile sig_atomic_t stop = 0;
static char resp_path[64];

static void on_stop(int sig) { (void)sig; stop = 1; }

static void cleanup(void)
{
    unlink(resp_path);
}

static void print_help(void)
{
    printf("commands:\n"
           "  ping                 check the pipeline end to end\n"
           "  sort n1 n2 ...       sort up to %d integers\n"
           "  primes lo hi         primes in [lo, hi]\n"
           "  fib n                first n Fibonacci numbers (n <= %d)\n"
           "  isprime n            is n prime?\n"
           "  help | quit\n", MAX_ARGS, MAX_FIB_N);
}

static int parse_int(const char *s, int64_t *out)
{
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

/* Returns 0 for a valid job request, 1 for quit, -1 for nothing to send. */
static int parse_command(char *line, client_request_t *req)
{
    char *tok = strtok(line, " \t\n");
    if (!tok)
        return -1;
    if (strcmp(tok, "quit") == 0 || strcmp(tok, "exit") == 0)
        return 1;
    if (strcmp(tok, "help") == 0) {
        print_help();
        return -1;
    }

    static const struct { const char *name; job_type_t type; } cmds[] = {
        { "ping", JOB_PING }, { "sort", JOB_SORT }, { "primes", JOB_PRIMES_RANGE },
        { "fib", JOB_FIB }, { "isprime", JOB_IS_PRIME },
    };
    int found = 0;
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        if (strcmp(tok, cmds[i].name) == 0) {
            req->type = cmds[i].type;
            found = 1;
        }
    }
    if (!found) {
        printf("unknown command '%s' (try help)\n", tok);
        return -1;
    }

    req->nargs = 0;
    while ((tok = strtok(NULL, " \t\n")) != NULL) {
        if (req->nargs == MAX_ARGS) {
            printf("too many arguments (max %d)\n", MAX_ARGS);
            return -1;
        }
        if (parse_int(tok, &req->args[req->nargs]) == -1) {
            printf("'%s' is not an integer\n", tok);
            return -1;
        }
        req->nargs++;
    }
    if (!job_args_ok(req->type, req->nargs)) {
        printf("wrong number of arguments for %s (try help)\n", job_type_name(req->type));
        return -1;
    }
    if (req->type == JOB_PRIMES_RANGE && req->args[0] > req->args[1]) {
        printf("primes: lo must be <= hi\n");
        return -1;
    }
    if (req->type == JOB_FIB && (req->args[0] < 0 || req->args[0] > MAX_FIB_N)) {
        printf("fib: n must be in [0, %d]\n", MAX_FIB_N);
        return -1;
    }
    return 0;
}

int main(void)
{
    install_handler(SIGINT, on_stop);
    install_handler(SIGTERM, on_stop);
    signal(SIGPIPE, SIG_IGN);             /* report a dead dispatcher via EPIPE instead of dying */

    snprintf(resp_path, sizeof resp_path, RESP_FIFO_FMT, (int)getpid());
    unlink(resp_path);
    if (mkfifo(resp_path, 0600) == -1)
        die("mkfifo response fifo");
    atexit(cleanup);

    int req_fd = open(REQ_FIFO, O_WRONLY);
    if (req_fd == -1) {
        if (errno == ENOENT)
            fprintf(stderr, "%s does not exist - start the dispatcher first\n", REQ_FIFO);
        else
            perror("open " REQ_FIFO);
        return 1;
    }
    /*
     * O_RDWR keeps the FIFO open for the client's lifetime: the dispatcher's
     * non-blocking open always finds a reader, and read() blocks for data
     * instead of returning EOF when no writer is connected.
     */
    int resp_fd = open(resp_path, O_RDWR);
    if (resp_fd == -1)
        die("open response fifo");

    printf("client pid %d connected to dispatcher (type 'help')\n", (int)getpid());
    char line[1024];
    while (!stop) {
        printf("job> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;                        /* EOF or interrupted by SIGINT */

        client_request_t req;
        memset(&req, 0, sizeof req);
        req.client_pid = getpid();
        int rc = parse_command(line, &req);
        if (rc == 1)
            break;
        if (rc == -1)
            continue;

        if (write(req_fd, &req, sizeof req) != (ssize_t)sizeof req) {
            perror("send request (is the dispatcher running?)");
            break;
        }

        client_response_t resp;
        ssize_t n;
        while ((n = read(resp_fd, &resp, sizeof resp)) == -1 && errno == EINTR && !stop)
            ;
        if (n != (ssize_t)sizeof resp) {
            if (!stop)
                fprintf(stderr, "bad response from dispatcher\n");
            break;
        }
        printf("[job %d] %s %s: %s\n", resp.job_id, job_type_name(resp.type),
               job_status_name(resp.status), resp.summary);
    }

    close(req_fd);
    close(resp_fd);
    return 0;
}
