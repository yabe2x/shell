#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* lexer.c */

typedef enum {
    T_WORD,
    T_PIPE,        /* |   */
    T_AMP,         /* &   */
    T_SEMI,        /* ;   */
    T_AND,         /* &&  */
    T_OR,          /* ||  */
    T_IN,          /* <   */
    T_OUT,         /* >   */
    T_APPEND,      /* >>  */
    T_ERR,         /* 2>  */
    T_ERR_APPEND,  /* 2>> */
    T_ERR_TO_OUT,  /* 2>&1 */
    T_END
} TokenType;

typedef struct {
    TokenType type;
    char **words;  /* T_WORD only; a glob can expand to many */
    int nwords;
    size_t start, end;
} Token;

typedef struct {
    Token *tokens;
    int count;
} TokenList;

typedef struct {
    int last_status;
    pid_t shell_pid;
    pid_t last_bg_pid;
} ExpandContext;

bool tokenize(const char *line, const ExpandContext *ctx, TokenList *out);
void free_tokens(TokenList *list);

/* parser.c */

typedef enum { R_IN, R_OUT, R_APPEND, R_ERR, R_ERR_APPEND, R_ERR_TO_OUT } RedirType;

typedef struct {
    RedirType type;
    char *file;
} Redir;

typedef struct {
    char **argv;
    int argc;
    Redir *redirs;
    int nredirs;
} Command;

typedef struct {
    Command *cmds;
    int ncmds;
    bool background;
    char *text;
} Pipeline;

typedef enum { RUN_ALWAYS, RUN_IF_OK, RUN_IF_FAILED } Connector;  /* ;  &&  || */

typedef struct {
    Pipeline *pipelines;
    Connector *connectors;
    int count;
} CommandList;

bool parse(const TokenList *tokens, const char *line, CommandList *out);
void free_command_list(CommandList *list);

/* jobs.c */

typedef struct {
    pid_t pid;
    bool completed;
    bool stopped;
    int status;
} Process;

typedef struct Job {
    int id;
    pid_t pgid;
    Process *procs;
    int nprocs;
    char *text;
    bool background;
    bool notified_stop;
    struct Job *next;
} Job;

Job *job_add(pid_t pgid, const char *text, bool background);
void job_add_process(Job *job, pid_t pid);
void job_remove(Job *job);
Job *job_find(int id);
Job *job_latest(void);
Job *job_list(void);
bool job_is_completed(const Job *job);
bool job_is_stopped(const Job *job);
int job_exit_status(const Job *job);

void job_mark_status(pid_t pid, int status);

void jobs_reap_and_notify(bool print);

/* exec.c */

typedef struct {
    bool interactive;
    int terminal;
    pid_t pgid;
    int last_status;   /* $? */
    pid_t last_bg_pid; /* $! */
    bool exit_requested;
    int exit_code;
} Shell;

extern Shell shell;

void shell_init(void);
int run_line(const char *line);
void wait_for_job(Job *job);
int put_job_in_foreground(Job *job, bool cont);
void put_job_in_background(Job *job, bool cont);
void signal_job(Job *job, int sig);

/* builtins.c */

bool is_builtin(const char *name);
int run_builtin(Command *cmd);

#endif
