# ysh: a Unix shell in C

A POSIX-style command-line shell written from scratch in about 1,400 lines of C. It has its own
tokenizer and parser, pipelines of any length, I/O redirection, `&&` / `||` / `;`, and
full job control (Ctrl-C, Ctrl-Z, `jobs`, `fg`, `bg`), built on `fork`, `execvp`, `pipe`,
`dup2`, `waitpid`, process groups, and `tcsetpgrp`.

```
ysh:~/project$ cat log.txt | grep ERROR | sort | uniq -c > summary.txt 2>&1
ysh:~/project$ make test && echo "all good" || echo "something broke"
ysh:~/project$ sleep 60
^Z
[1]+  Stopped                sleep 60
ysh:~/project$ bg
[1]+ sleep 60 &
ysh:~/project$ jobs
[1]+  Running                sleep 60 &
```

## Features

| | |
|---|---|
| **Pipes** | `a | b | c | ...`, any length. The exit status is the last command's, like bash. |
| **Redirection** | `<`  `>`  `>>`  `2>`  `2>>`  `2>&1`, applied left to right (so `> f 2>&1` and `2>&1 > f` differ, like bash) |
| **Sequencing** | `;`  `&&`  `||`  `&` |
| **Quoting** | `'single'` (literal), `"double"` (expands `$`), and `\` escapes |
| **Expansion** | `$VAR`, `${VAR}`, `$?`, `$$`, `$!`, `~`, and wildcards `* ? [...]` |
| **Job control** | Ctrl-C and Ctrl-Z reach the whole foreground pipeline but never the shell. Also `jobs`, `fg %n`, `bg %n`, `wait`, and "Done" notices for finished background jobs. |
| **Built-ins** | `cd` (and `cd -`), `pwd`, `exit [n]`, `export`, `unset`, `jobs`, `fg`, `bg`, `wait`, `help` |
| **Modes** | interactive (prompt and job control), `ysh script.sh`, `ysh -c "cmd"`, or commands piped in on stdin |

## How it works

```
"cat a.txt | grep x > out &"
        │
   lexer.c ───► WORD(cat) WORD(a.txt) PIPE WORD(grep) WORD(x) OUT WORD(out) AMP
        │
   parser.c ──► Pipeline{ [cat a.txt] | [grep x, stdout→out] }, background
        │
   exec.c ───► pipe() + fork() per command, dup2() the pipe ends and files, execvp()
        │
   jobs.c ───► job table: one entry per pipeline, updated from waitpid()
```

- **Two-pass execution.** The whole line is tokenized and parsed once to check syntax. Then
  each pipeline is expanded again right before it runs. That's why `false; echo $?` prints
  `1` and `cd /tmp; echo $PWD` prints `/tmp`. Expanding everything up front would show
  stale values.
- **Pipes.** For each command the shell makes a pipe, forks, and in the child connects stdin to
  the previous pipe and stdout to the next one. The parent closes its copies of each pipe end
  right away, because if any write end stays open, the reader never sees end-of-file and the
  pipeline hangs. A test checks that `ls /proc/self/fd` sees exactly the same open files under
  ysh as under bash, so no descriptors leak.
- **Process groups.** Each pipeline gets its own process group (`setpgid`), and the
  foreground one gets the terminal (`tcsetpgrp`). When you press Ctrl-C, the terminal
  signals that whole group: every process in the pipeline, but not the shell. Both parent
  and child call `setpgid`, which avoids a race over which one runs first after `fork()`.
- **Waiting.** The shell waits with `waitpid(-1, ..., WUNTRACED)` and files each result under
  the job that owns that process. A background job finishing while you wait on a foreground
  one is recorded and reported at the next prompt. `WUNTRACED` is how a Ctrl-Z'd job shows up
  as "Stopped" instead of the shell hanging forever.
- **Built-ins run in the shell.** `cd` has to change the shell's own directory, and a child
  process can't do that for its parent. Redirections on built-ins (`pwd > f`) save stdin,
  stdout, and stderr with `dup`, redirect, run, and restore them.
- **Signals.** The shell ignores `SIGTSTP`/`SIGTTOU`/... itself, and every child resets
  them to default before `exec`. At the prompt, Ctrl-C is caught (not ignored) so it clears
  the half-typed line, like bash.

## Testing

```bash
make test
```

- **`tests/run_tests.sh`** runs 85 command lines through both **bash and ysh** and requires
  identical output and exit codes. It covers quoting, wildcards, pipes, every redirect,
  `&&`/`||`, built-ins, background jobs, syntax errors, scripts, file-descriptor leaks, and edge cases like a 100-command pipeline.
- The same 85 tests run again on a build with **AddressSanitizer + UBSan**, so a memory error,
  leak, or undefined behavior anywhere fails the test.
- **`tests/test_interactive.py`** runs ysh inside a real **pseudo-terminal** and presses keys:
  Ctrl-C at the prompt, Ctrl-C on a 3-process pipeline, Ctrl-Z → `jobs` → `bg` → `fg` → Ctrl-C,
  background "Done" notices, a foreground program reading the keyboard, and Ctrl-D / `exit`.

Result: 85/85 (normal), 85/85 (sanitizers), 9/9 (interactive).

## Not supported (yet)

Subshells `( )`, command substitution `$(...)`, here-documents `<<`, `if`/`for`/`while`,
word splitting of unquoted variables, and line editing / history. These would be the next steps.

## Build

Linux or WSL with `gcc`, `make`, and `python3` (for the interactive tests).

```bash
make        # builds ./ysh
./ysh
```

| File | What it is |
|---|---|
| `src/lexer.c` | Tokenizer: quotes, escapes, `$` expansion, `~`, wildcards |
| `src/parser.c` | Tokens → command list → pipelines → commands + redirects |
| `src/exec.c` | fork/exec, pipes, redirection, process groups, waiting, foreground/background |
| `src/jobs.c` | Job table and status tracking |
| `src/builtins.c` | cd, pwd, exit, jobs, fg, bg, wait, export, unset, help |
| `src/main.c` | Prompt and read loop, `-c` and script modes, Ctrl-C at the prompt |
