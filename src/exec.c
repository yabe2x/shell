/* Job control follows the design in the GNU libc manual. It's only enabled when
 * stdin is a terminal. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "shell.h"

Shell shell;
static struct termios shell_tmodes;

void shell_init(void) {
    shell.interactive = isatty(STDIN_FILENO);
    shell.terminal = STDIN_FILENO;

    if (shell.interactive) {
        while (tcgetpgrp(shell.terminal) != (shell.pgid = getpgrp())) kill(-shell.pgid, SIGTTIN);

        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);

        shell.pgid = getpid();
        if (setpgid(shell.pgid, shell.pgid) < 0 && errno != EPERM) perror("ysh: setpgid");
        tcsetpgrp(shell.terminal, shell.pgid);
        tcgetattr(shell.terminal, &shell_tmodes);
    } else {
        shell.pgid = getpgrp();
    }
}

/* applied left to right, like bash */
static bool apply_redirects(const Command *cmd) {
    for (int i = 0; i < cmd->nredirs; i++) {
        const Redir *r = &cmd->redirs[i];
        if (r->type == R_ERR_TO_OUT) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
            continue;
        }
        int flags, target;
        switch (r->type) {
        case R_IN: flags = O_RDONLY, target = STDIN_FILENO; break;
        case R_OUT: flags = O_WRONLY | O_CREAT | O_TRUNC, target = STDOUT_FILENO; break;
        case R_APPEND: flags = O_WRONLY | O_CREAT | O_APPEND, target = STDOUT_FILENO; break;
        case R_ERR: flags = O_WRONLY | O_CREAT | O_TRUNC, target = STDERR_FILENO; break;
        default: flags = O_WRONLY | O_CREAT | O_APPEND, target = STDERR_FILENO; break;
        }
        int fd = open(r->file, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "ysh: %s: %s\n", r->file, strerror(errno));
            return false;
        }
        if (fd != target) {
            dup2(fd, target);
            close(fd);
        }
    }
    return true;
}

/* runs in the child; never returns */
static void exec_command(Command *cmd) {
    if (!apply_redirects(cmd)) _exit(1);
    if (cmd->argc == 0) _exit(0);
    if (is_builtin(cmd->argv[0])) {
        int status = run_builtin(cmd);
        fflush(stdout);
        _exit(status);
    }
    execvp(cmd->argv[0], cmd->argv);
    if (errno == ENOENT) {
        fprintf(stderr, "ysh: %s: command not found\n", cmd->argv[0]);
        _exit(127);
    }
    fprintf(stderr, "ysh: %s: %s\n", cmd->argv[0], strerror(errno));
    _exit(126);
}

/* builtins run in the shell process, with redirects applied temporarily */
static int run_in_shell(Command *cmd) {
    fflush(stdout);
    int saved[3];
    for (int fd = 0; fd < 3; fd++) saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 10);

    int status = 1;
    if (apply_redirects(cmd)) status = cmd->argc ? run_builtin(cmd) : 0;

    fflush(stdout);
    fflush(stderr);
    for (int fd = 0; fd < 3; fd++) {
        if (saved[fd] >= 0) {
            dup2(saved[fd], fd);
            close(saved[fd]);
        }
    }
    return status;
}

static void reset_child_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

/* The parent closes each pipe end as soon as it's handed off, or readers never see EOF. */
static int run_pipeline(Pipeline *pl) {
    Command *first = &pl->cmds[0];
    if (pl->ncmds == 1 && !pl->background && (first->argc == 0 || is_builtin(first->argv[0])))
        return run_in_shell(first);

    Job *job = job_add(0, pl->text, pl->background);
    pid_t pgid = 0;
    int in_fd = -1;

    for (int i = 0; i < pl->ncmds; i++) {
        int pfd[2] = {-1, -1};
        if (i < pl->ncmds - 1 && pipe(pfd) < 0) {
            perror("ysh: pipe");
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("ysh: fork");
            if (pfd[0] >= 0) close(pfd[0]), close(pfd[1]);
            break;
        }

        if (pid == 0) {
            if (shell.interactive) {
                pid_t mypg = pgid ? pgid : getpid();
                setpgid(0, mypg);
                if (!pl->background) tcsetpgrp(shell.terminal, mypg);
            }
            reset_child_signals();
            if (!shell.interactive && pl->background) {
                /* like sh: non-interactive background jobs ignore SIGINT and get /dev/null as stdin */
                signal(SIGINT, SIG_IGN);
                signal(SIGQUIT, SIG_IGN);
                if (i == 0) {
                    int devnull = open("/dev/null", O_RDONLY);
                    if (devnull >= 0) dup2(devnull, STDIN_FILENO), close(devnull);
                }
            }
            if (in_fd != -1) {
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }
            if (pfd[1] != -1) {
                dup2(pfd[1], STDOUT_FILENO);
                close(pfd[1]);
                close(pfd[0]);
            }
            exec_command(&pl->cmds[i]);
        }

        if (!pgid) pgid = pid;
        if (shell.interactive) setpgid(pid, pgid);
        job_add_process(job, pid);
        if (in_fd != -1) close(in_fd);
        if (pfd[1] != -1) close(pfd[1]);
        in_fd = pfd[0];
    }
    if (in_fd != -1) close(in_fd);
    job->pgid = shell.interactive ? pgid : shell.pgid;

    if (job->nprocs == 0) {
        job_remove(job);
        return 1;
    }
    if (pl->background) {
        shell.last_bg_pid = job->procs[job->nprocs - 1].pid;
        if (shell.interactive) printf("[%d] %d\n", job->id, (int)shell.last_bg_pid);
        return 0;
    }
    return put_job_in_foreground(job, false);
}

void signal_job(Job *job, int sig) {
    if (shell.interactive) {
        kill(-job->pgid, sig);
    } else {
        /* no separate process group without job control */
        for (int i = 0; i < job->nprocs; i++)
            if (!job->procs[i].completed) kill(job->procs[i].pid, sig);
    }
}

static void mark_running(Job *job) {
    for (int i = 0; i < job->nprocs; i++) job->procs[i].stopped = false;
    job->notified_stop = false;
}

void wait_for_job(Job *job) {
    /* waitpid(-1) can also return background children; job_mark_status records those too */
    while (!job_is_completed(job) && !job_is_stopped(job)) {
        int status;
        pid_t pid = waitpid(-1, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        job_mark_status(pid, status);
    }
}

int put_job_in_foreground(Job *job, bool cont) {
    job->background = false;
    if (shell.interactive) tcsetpgrp(shell.terminal, job->pgid);
    if (cont) {
        mark_running(job);
        signal_job(job, SIGCONT);
    }

    wait_for_job(job);

    if (shell.interactive) {
        tcsetpgrp(shell.terminal, shell.pgid);
        tcsetattr(shell.terminal, TCSADRAIN, &shell_tmodes);
    }

    if (job_is_stopped(job)) {
        printf("\n[%d]+  %-22s %s\n", job->id, "Stopped", job->text);
        fflush(stdout);
        job->notified_stop = true;
        job->background = true;
        return 128 + SIGTSTP;
    }

    int status = job_exit_status(job);
    int last = job->procs[job->nprocs - 1].status;
    if (WIFSIGNALED(last)) {
        int sig = WTERMSIG(last);
        if (sig == SIGINT) {
            if (shell.interactive) putchar('\n');
        } else if (sig != SIGPIPE) {
            fprintf(stderr, "%s%s\n", strsignal(sig), WCOREDUMP(last) ? " (core dumped)" : "");
        }
    }
    job_remove(job);
    return status;
}

void put_job_in_background(Job *job, bool cont) {
    job->background = true;
    if (cont) {
        mark_running(job);
        signal_job(job, SIGCONT);
    }
}

/* Expanded right before running so $? and earlier cd/export on the same line are seen. */
static int run_pipeline_text(const char *text, bool background) {
    ExpandContext ctx = {shell.last_status, getpid(), shell.last_bg_pid};
    TokenList tokens;
    if (!tokenize(text, &ctx, &tokens)) return 2;

    CommandList list;
    int status = 2;
    if (parse(&tokens, text, &list)) {
        if (list.count == 1) {
            list.pipelines[0].background = background;
            status = run_pipeline(&list.pipelines[0]);
        }
        free_command_list(&list);
    }
    free_tokens(&tokens);
    return status;
}

int run_line(const char *line) {
    /* syntax-check the whole line first */
    ExpandContext ctx = {shell.last_status, getpid(), shell.last_bg_pid};
    TokenList tokens;
    if (!tokenize(line, &ctx, &tokens)) return shell.last_status = 2;
    CommandList list;
    bool ok = parse(&tokens, line, &list);
    free_tokens(&tokens);
    if (!ok) return shell.last_status = 2;

    for (int i = 0; i < list.count && !shell.exit_requested; i++) {
        if (list.connectors[i] == RUN_IF_OK && shell.last_status != 0) continue;
        if (list.connectors[i] == RUN_IF_FAILED && shell.last_status == 0) continue;
        shell.last_status = run_pipeline_text(list.pipelines[i].text, list.pipelines[i].background);
    }
    free_command_list(&list);
    return shell.last_status;
}
