#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shell.h"

extern char **environ;

static int b_cd(Command *cmd) {
    const char *dir = cmd->argc > 1 ? cmd->argv[1] : getenv("HOME");
    bool print = false;
    if (cmd->argc > 2) {
        fprintf(stderr, "ysh: cd: too many arguments\n");
        return 1;
    }
    if (dir && strcmp(dir, "-") == 0) {
        dir = getenv("OLDPWD");
        print = true;
        if (!dir) {
            fprintf(stderr, "ysh: cd: OLDPWD not set\n");
            return 1;
        }
    }
    if (!dir) {
        fprintf(stderr, "ysh: cd: HOME not set\n");
        return 1;
    }

    char old[PATH_MAX];
    if (!getcwd(old, sizeof(old))) old[0] = '\0';
    if (chdir(dir) < 0) {
        fprintf(stderr, "ysh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    char now[PATH_MAX];
    if (getcwd(now, sizeof(now))) setenv("PWD", now, 1);
    if (old[0]) setenv("OLDPWD", old, 1);
    if (print) printf("%s\n", now);
    return 0;
}

static int b_pwd(Command *cmd) {
    (void)cmd;
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) {
        perror("ysh: pwd");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}

static int b_exit(Command *cmd) {
    int code = shell.last_status;
    if (cmd->argc > 1) {
        char *end;
        long n = strtol(cmd->argv[1], &end, 10);
        if (*cmd->argv[1] == '\0' || *end != '\0') {
            fprintf(stderr, "ysh: exit: %s: numeric argument required\n", cmd->argv[1]);
            code = 2;
        } else {
            code = (int)(n & 0xFF);
        }
    }
    shell.exit_requested = true;
    shell.exit_code = code;
    return code;
}

/* "%2", "2", or nothing for the most recent job */
static Job *parse_job_spec(const char *builtin, int argc, char **argv) {
    if (argc < 2) {
        Job *j = job_latest();
        if (!j) fprintf(stderr, "ysh: %s: current: no such job\n", builtin);
        return j;
    }
    const char *spec = argv[1][0] == '%' ? argv[1] + 1 : argv[1];
    Job *j = isdigit((unsigned char)spec[0]) ? job_find(atoi(spec)) : NULL;
    if (!j) fprintf(stderr, "ysh: %s: %s: no such job\n", builtin, argv[1]);
    return j;
}

static int b_jobs(Command *cmd) {
    (void)cmd;
    jobs_reap_and_notify(true);
    Job *latest = job_latest();
    for (Job *j = job_list(); j; j = j->next) {
        printf("[%d]%c  %-22s %s%s\n", j->id, j == latest ? '+' : ' ', job_is_stopped(j) ? "Stopped" : "Running",
               j->text, job_is_stopped(j) ? "" : " &");
    }
    return 0;
}

static int b_fg(Command *cmd) {
    if (!shell.interactive) {
        fprintf(stderr, "ysh: fg: no job control\n");
        return 1;
    }
    Job *j = parse_job_spec("fg", cmd->argc, cmd->argv);
    if (!j) return 1;
    printf("%s\n", j->text);
    fflush(stdout);
    return put_job_in_foreground(j, true);
}

static int b_bg(Command *cmd) {
    if (!shell.interactive) {
        fprintf(stderr, "ysh: bg: no job control\n");
        return 1;
    }
    Job *j = parse_job_spec("bg", cmd->argc, cmd->argv);
    if (!j) return 1;
    put_job_in_background(j, true);
    printf("[%d]+ %s &\n", j->id, j->text);
    return 0;
}

static int b_wait(Command *cmd) {
    int status = 0;
    if (cmd->argc > 1) {
        Job *j = parse_job_spec("wait", cmd->argc, cmd->argv);
        if (!j) return 127;
        wait_for_job(j);
        if (job_is_completed(j)) {
            status = job_exit_status(j);
            job_remove(j);
        }
        return status;
    }
    Job *j = job_list();
    while (j) {
        Job *next = j->next;
        if (!job_is_stopped(j)) {
            wait_for_job(j);
            if (job_is_completed(j)) {
                status = job_exit_status(j);
                job_remove(j);
            }
        }
        j = next;
    }
    return status;
}

static int b_export(Command *cmd) {
    if (cmd->argc == 1) {
        for (char **e = environ; *e; e++) printf("export %s\n", *e);
        return 0;
    }
    int status = 0;
    for (int i = 1; i < cmd->argc; i++) {
        char *eq = strchr(cmd->argv[i], '=');
        if (!eq) continue;
        *eq = '\0';
        if (cmd->argv[i][0] == '\0' || setenv(cmd->argv[i], eq + 1, 1) < 0) {
            fprintf(stderr, "ysh: export: `%s': not a valid identifier\n", cmd->argv[i]);
            status = 1;
        }
        *eq = '=';
    }
    return status;
}

static int b_unset(Command *cmd) {
    for (int i = 1; i < cmd->argc; i++) unsetenv(cmd->argv[i]);
    return 0;
}

static int b_help(Command *cmd);

typedef struct {
    const char *name;
    int (*fn)(Command *);
    const char *usage;
} Builtin;

static const Builtin builtins[] = {
    {"cd", b_cd, "cd [dir | -]        change directory"},
    {"pwd", b_pwd, "pwd                 print the current directory"},
    {"exit", b_exit, "exit [n]            leave the shell"},
    {"jobs", b_jobs, "jobs                list background and stopped jobs"},
    {"fg", b_fg, "fg [%n]             bring a job to the foreground"},
    {"bg", b_bg, "bg [%n]             resume a stopped job in the background"},
    {"wait", b_wait, "wait [%n]           wait for background jobs to finish"},
    {"export", b_export, "export NAME=value   set an environment variable"},
    {"unset", b_unset, "unset NAME          remove an environment variable"},
    {"help", b_help, "help                show this list"},
};
static const int NUM_BUILTINS = sizeof(builtins) / sizeof(builtins[0]);

static int b_help(Command *cmd) {
    (void)cmd;
    printf("ysh built-in commands:\n");
    for (int i = 0; i < NUM_BUILTINS; i++) printf("  %s\n", builtins[i].usage);
    printf("Also: pipes |, redirects < > >> 2> 2>> 2>&1, background &, ; && ||,\n"
           "quotes '' \"\", $VAR $? $$ $!, ~, and wildcards * ? [].\n");
    return 0;
}

bool is_builtin(const char *name) {
    for (int i = 0; i < NUM_BUILTINS; i++)
        if (strcmp(name, builtins[i].name) == 0) return true;
    return false;
}

int run_builtin(Command *cmd) {
    for (int i = 0; i < NUM_BUILTINS; i++)
        if (strcmp(cmd->argv[0], builtins[i].name) == 0) return builtins[i].fn(cmd);
    return 127;
}
