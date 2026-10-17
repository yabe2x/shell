#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "shell.h"

static Job *jobs = NULL;

Job *job_list(void) { return jobs; }

Job *job_add(pid_t pgid, const char *text, bool background) {
    Job *job = calloc(1, sizeof(Job));
    int max_id = 0;
    for (Job *j = jobs; j; j = j->next)
        if (j->id > max_id) max_id = j->id;
    job->id = max_id + 1;
    job->pgid = pgid;
    job->text = strdup(text);
    job->background = background;

    Job **tail = &jobs;
    while (*tail) tail = &(*tail)->next;
    *tail = job;
    return job;
}

void job_add_process(Job *job, pid_t pid) {
    job->procs = realloc(job->procs, (job->nprocs + 1) * sizeof(Process));
    job->procs[job->nprocs++] = (Process){.pid = pid};
}

void job_remove(Job *job) {
    for (Job **p = &jobs; *p; p = &(*p)->next) {
        if (*p == job) {
            *p = job->next;
            break;
        }
    }
    free(job->procs);
    free(job->text);
    free(job);
}

Job *job_find(int id) {
    for (Job *j = jobs; j; j = j->next)
        if (j->id == id) return j;
    return NULL;
}

Job *job_latest(void) {
    Job *last = NULL;
    for (Job *j = jobs; j; j = j->next) last = j;
    return last;
}

bool job_is_completed(const Job *job) {
    for (int i = 0; i < job->nprocs; i++)
        if (!job->procs[i].completed) return false;
    return true;
}

bool job_is_stopped(const Job *job) {
    bool any_stopped = false;
    for (int i = 0; i < job->nprocs; i++) {
        if (!job->procs[i].completed && !job->procs[i].stopped) return false;
        if (job->procs[i].stopped) any_stopped = true;
    }
    return any_stopped;
}

int job_exit_status(const Job *job) {
    if (job->nprocs == 0) return 0;
    int st = job->procs[job->nprocs - 1].status;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 0;
}

void job_mark_status(pid_t pid, int status) {
    for (Job *j = jobs; j; j = j->next) {
        for (int i = 0; i < j->nprocs; i++) {
            Process *p = &j->procs[i];
            if (p->pid != pid) continue;
            if (WIFSTOPPED(status)) {
                p->stopped = true;
            } else if (WIFCONTINUED(status)) {
                p->stopped = false;
                j->notified_stop = false;
            } else {
                p->completed = true;
                p->stopped = false;
                p->status = status;
            }
            return;
        }
    }
}

static const char *done_word(const Job *job, char *buf, size_t n) {
    int st = job->procs[job->nprocs - 1].status;
    if (WIFSIGNALED(st)) {
        snprintf(buf, n, "%s", strsignal(WTERMSIG(st)));
        return buf;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        snprintf(buf, n, "Exit %d", WEXITSTATUS(st));
        return buf;
    }
    return "Done";
}

void jobs_reap_and_notify(bool print) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) job_mark_status(pid, status);

    Job *j = jobs;
    while (j) {
        Job *next = j->next;
        if (job_is_completed(j)) {
            if (print) {
                char buf[64];
                printf("[%d]%c  %-22s %s\n", j->id, j == job_latest() ? '+' : ' ', done_word(j, buf, sizeof(buf)),
                       j->text);
            }
            job_remove(j);
        } else if (job_is_stopped(j) && !j->notified_stop) {
            if (print) printf("[%d]%c  %-22s %s\n", j->id, j == job_latest() ? '+' : ' ', "Stopped", j->text);
            j->notified_stop = true;
        }
        j = next;
    }
    fflush(stdout);
}
