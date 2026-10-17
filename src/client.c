// client.c - the program the user types commands into.
//
// Reads a command, sends it to the dispatcher through the shared request
// pipe, and waits for the reply on this client's own reply pipe. It sends
// one job at a time.
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
           "  primes lo hi         primes in [lo, hi] (hi <= 10^12, hi - lo <= 10^7)\n"
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

// Returns 0 if line is a valid job, 1 if the user wants to quit, -1 if there is nothing to send.
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
    char err[SUMMARY_LEN];
    if (job_validate(req->type, req->nargs, req->args, err, sizeof err) == -1) {
        printf("%s (try help)\n", err);
        return -1;
    }
    return 0;
}

// Print the result values the dispatcher sent along, 10 per line
static void print_data(const client_response_t *resp)
{
    if (resp->type == JOB_IS_PRIME || resp->count == 0)
        return;
    for (int i = 0; i < resp->count; i++)
        printf("%s%lld", i % 10 == 0 ? "\n    " : " ", (long long)resp->data[i]);
    if (resp->length > (size_t)resp->count)
        printf(" ... (%zu more)", resp->length - resp->count);
    printf("\n");
}

int main(void)
{
    // attach custom signal handlers
    install_handler(SIGINT, on_stop);
    install_handler(SIGTERM, on_stop);
    signal(SIGPIPE, SIG_IGN);             // if the dispatcher is gone, print an error instead of crashing

    snprintf(resp_path, sizeof resp_path, RESP_FIFO_FMT, (int)getpid());
    unlink(resp_path);
    if (mkfifo(resp_path, 0600) == -1)
        die("mkfifo response fifo");
    atexit(cleanup);


    // the dispatcher has to be running first, since it creates the request pipe
    int req_fd = open(REQ_FIFO, O_WRONLY);
    if (req_fd == -1) {
        if (errno == ENOENT)
            fprintf(stderr, "%s does not exist - start the dispatcher first\n", REQ_FIFO);
        else
            perror("open " REQ_FIFO);
        return 1;
    }
    // Open our reply pipe for both reading and writing so it stays open the
    // whole time. That way the dispatcher can always connect to it, and our
    // reads wait for a reply instead of ending early when nobody is writing.
    int resp_fd = open(resp_path, O_RDWR);
    if (resp_fd == -1)
        die("open response fifo");

    printf("client pid %d connected to dispatcher (type 'help')\n", (int)getpid());
    char line[1024];
    while (!stop) {
        printf("job> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;                        // input ended, or the user pressed Ctrl+C

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
        print_data(&resp);
    }

    close(req_fd);
    close(resp_fd);
    return 0;
}
