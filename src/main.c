/* usage: ysh [script | -c command] */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

static volatile sig_atomic_t got_sigint = 0;
static void on_sigint(int sig) {
    (void)sig;
    got_sigint = 1;
}

static void print_prompt(void) {
    char cwd[PATH_MAX];
    const char *home = getenv("HOME");
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    if (home && home[0] && strncmp(cwd, home, strlen(home)) == 0 &&
        (cwd[strlen(home)] == '/' || cwd[strlen(home)] == '\0'))
        printf("ysh:~%s$ ", cwd + strlen(home));
    else
        printf("ysh:%s$ ", cwd);
    fflush(stdout);
}

static int run_stream(FILE *in) {
    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        if (shell.interactive) {
            jobs_reap_and_notify(true);
            print_prompt();
        }

        errno = 0;
        ssize_t n = getline(&line, &cap, in);
        if (n < 0) {
            if (errno == EINTR && got_sigint) {
                got_sigint = 0;
                clearerr(in);
                putchar('\n');
                continue;
            }
            if (shell.interactive) printf("exit\n");
            break;
        }
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';

        run_line(line);
        if (!shell.interactive) jobs_reap_and_notify(false);
        if (shell.exit_requested) break;
    }
    free(line);
    return shell.exit_requested ? shell.exit_code : shell.last_status;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        shell.interactive = false;
        shell.pgid = getpgrp();
        run_line(argv[2]);
        return shell.exit_requested ? shell.exit_code : shell.last_status;
    }

    if (argc >= 2) {
        FILE *f = fopen(argv[1], "re");
        if (!f) {
            fprintf(stderr, "ysh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
        shell.interactive = false;
        shell.pgid = getpgrp();
        int status = run_stream(f);
        fclose(f);
        return status;
    }

    shell_init();
    if (shell.interactive) {
        /* no SA_RESTART so Ctrl-C interrupts getline() */
        struct sigaction sa = {0};
        sa.sa_handler = on_sigint;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
    }
    return run_stream(stdin);
}
