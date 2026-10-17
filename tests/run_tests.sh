#!/bin/bash
# Compares ysh's stdout and exit status against bash for each case.
# Usage: tests/run_tests.sh ./ysh

YSH=$(realpath "$1")
PASS=0
FAIL=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
setup() {
    rm -rf "$WORK"/*
    cd "$WORK" || exit 1
    printf 'apple\nbanana\ncherry\napple pie\n' > a.txt
    printf 'zebra\nmango\nkiwi\n' > b.txt
    printf 'int main(void) { return 0; }\n' > c.c
    printf '3\n1\n2\n' > unsorted.txt
    mkdir -p sub/deeper
}

# check "description" 'command line' [status-only]
check() {
    local name="$1" cmd="$2" mode="$3"
    setup; local want_out; want_out=$(bash -c "$cmd" 2>/dev/null </dev/null); local want_status=$?
    setup; local got_out; got_out=$(timeout 10 "$YSH" -c "$cmd" 2>"$WORK/../ysh_err_$$" </dev/null); local got_status=$?
    local err; err=$(cat "$WORK/../ysh_err_$$"); rm -f "$WORK/../ysh_err_$$"

    local ok=1
    [ "$got_status" = "$want_status" ] || ok=0
    [ "$mode" = "status-only" ] || [ "$got_out" = "$want_out" ] || ok=0
    if echo "$err" | grep -qE "AddressSanitizer|LeakSanitizer|runtime error"; then ok=0; fi

    if [ $ok = 1 ]; then
        PASS=$((PASS + 1))
        printf '  pass  %s\n' "$name"
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL  %s\n        command: %s\n' "$name" "$cmd"
        printf '        bash: status %s, output: %q\n' "$want_status" "$want_out"
        printf '        ysh:  status %s, output: %q\n' "$got_status" "$got_out"
        [ -n "$err" ] && printf '        ysh stderr: %s\n' "$(echo "$err" | head -5)"
    fi
}

echo "Basics"
check "simple command"              'echo hello'
check "arguments"                   'echo one two   three'
check "absolute path"               '/bin/echo via path'
check "exit status of true/false"   'false; echo $?; true; echo $?'
check "command not found = 127"     'no_such_command_xyz; echo $?'
check "not executable = 126"        'chmod -x c.c; ./c.c; echo $?'
check "comment"                     'echo visible # not this'

echo "Quoting and expansion"
check "double quotes keep spaces"   'echo "a   b    c"'
check "single quotes are literal"   "echo 'no \$HOME here'"
check "variables in double quotes"  'echo "home=$HOME"'
check "\${NAME}"                    'echo ${HOME}/x'
check "backslash escapes"           'echo a\ b \"q\" \$HOME'
check "escapes inside double quotes" 'echo "x \"y\" \$z \\ end"'
check "empty quoted argument"       'printf "[%s]" a "" b'
check "unset variable vanishes"     'printf "[%s]" a $NO_SUCH_VAR b'
check "mixed quotes in one word"    "echo 'one'\"two\"three"
check "\$\$ is a number"           'echo $$ | grep -c "^[0-9][0-9]*$"'
check "tilde"                       'echo ~ ~/docs'
check "quoted tilde stays"          'echo "~"'

echo "Wildcards"
check "*.txt"                       'echo *.txt'
check "?.c"                         'echo ?.c'
check "[ab].txt"                    'echo [ab].txt'
check "no match stays literal"      'echo nothing*.zzz'
check "quoted wildcard is literal"  'echo "*.txt"'
check "half-quoted wildcard"        'echo "a".*'

echo "Sequencing"
check "semicolons"                  'echo one; echo two;echo three'
check "&& runs on success"          'true && echo yes'
check "&& skips on failure"         'false && echo yes; echo after'
check "|| runs on failure"          'false || echo fallback'
check "|| skips on success"         'true || echo never; echo after'
check "chain of && and ||"          'false && echo a || echo b && echo c'
check "\$? updates mid-line"        'false; echo $?; true; echo $?'
check "export then use, same line"  'export GREETING=hi; echo $GREETING'

echo "Pipes"
check "two commands"                'cat a.txt | grep apple'
check "three commands"              'cat a.txt | grep apple | wc -l'
check "five commands"               'cat a.txt | cat | cat | sort | head -2'
check "pipe exit status = last"     'false | true; echo $?; true | false; echo $?'
check "large data through a pipe"   'seq 1 200000 | tail -1'
check "reader quits early (SIGPIPE)" 'yes | head -3'
check "builtin inside a pipe"       'pwd | wc -l'
check "no leaked file descriptors"  'ls /proc/self/fd | wc -l'
check "fds in a pipe's middle"      'echo | ls /proc/self/fd | wc -l'

echo "Redirection"
check "> creates and truncates"     'echo first > f; echo second > f; cat f'
check ">> appends"                  'echo one > f; echo two >> f; cat f'
check "< reads a file"              'sort < unsorted.txt'
check "< and > together"            'sort < unsorted.txt > sorted.txt; cat sorted.txt'
check "redirect before command"     '> f echo early; cat f'
check "2> sends errors to a file"   'ls no_such_file 2> err.txt; echo $?; wc -l < err.txt'
check "2>> appends errors"          'ls nope1 2> e; ls nope2 2>> e; wc -l < e'
check "2>&1 into a pipe"            'ls no_such_file 2>&1 | wc -l'
check "> file 2>&1 (order matters)" 'ls a.txt no_such_file > all.txt 2>&1; wc -l < all.txt'
check "redirect + pipe"             'grep apple < a.txt | tr a-z A-Z > up.txt; cat up.txt'
check "missing input file"          'cat < nope.txt; echo $?'
check "redirect on a builtin"       'pwd > p.txt; wc -l < p.txt'
check "redirect alone creates file" '> empty.txt; ls empty.txt'

echo "Edge cases"
LONG_PIPE="echo start$(printf ' | cat%.0s' $(seq 1 100)) | wc -c"
check "100-command pipeline"        "$LONG_PIPE"
check "exit inside a pipe only exits that child" 'exit 3 | cat; echo still here $?'
check "cd inside a pipe doesn't move the shell" 'cd sub | cat; pwd | sed "s|.*/||"'
check "blank line keeps \$?"        'false;    echo $?'
check "tabs between words"          "$(printf 'echo\ta\t\tb')"
check "operators without spaces"    'echo a|tr a b;echo c&&echo d||echo e'
check "2 as an argument, not 2>"    'echo 2 22 a2>f; cat f'
check "lone dollar sign"            'echo $ cost $5'
check "300 files from one wildcard" 'seq -f "file%03g" 1 300 | xargs touch; echo file* | wc -w'
check "deep directory"              'mkdir -p d1/d2/d3/d4/d5 && cd d1/d2/d3/d4/d5 && pwd | sed "s|.*/d1|d1|"'

echo "Built-ins"
check "cd and pwd"                  'cd sub/deeper && pwd | sed "s|.*/sub|sub|"'
check "cd with no args goes home"   'cd; pwd'
check "cd - goes back"              'cd sub; cd - > /dev/null; pwd | sed "s|.*/||"'
check "cd to missing dir fails"     'cd /no/such/dir; echo $?'
check "export / unset"              'export X1=abc; echo $X1; unset X1; echo "[$X1]"'
check "exit with a code"            'exit 7'
check "exit stops the line"         'echo before; exit 3; echo after'
check "exit uses last status"       'false; exit'

echo "Background jobs"
check "& doesn't wait"              'sleep 0.3 & echo right away; wait; echo after'
check "wait collects all jobs"      'sleep 0.2 & sleep 0.1 & wait; echo both done'
check "\$! is the last bg pid"      'sleep 0.1 & kill -0 $! && echo alive; wait'
check "background output appears"  'echo from bg & wait'

echo "Syntax errors (exit status 2)"
check "pipe with no command"        '| ls' status-only
check "double semicolon"            'echo a ;; echo b' status-only
check "&& at end of line"           'echo a &&' status-only
check "unclosed double quote"       'echo "oops' status-only
check "unclosed single quote"       "echo 'oops" status-only
check "redirect with no file"       'echo a >' status-only

echo "Scripts and stdin"
setup
printf 'echo line one\ncd sub\npwd | sed "s|.*/||"\nfalse\necho status $?\n' > script.sh
want=$(bash script.sh 2>&1); got=$("$YSH" script.sh 2>&1)
if [ "$want" = "$got" ]; then PASS=$((PASS + 1)); echo "  pass  run a script file"
else FAIL=$((FAIL + 1)); printf '  FAIL  run a script file\n        bash: %q\n        ysh:  %q\n' "$want" "$got"; fi
want=$(bash < script.sh 2>&1); got=$("$YSH" < script.sh 2>&1)
if [ "$want" = "$got" ]; then PASS=$((PASS + 1)); echo "  pass  read commands from stdin"
else FAIL=$((FAIL + 1)); printf '  FAIL  read commands from stdin\n        bash: %q\n        ysh:  %q\n' "$want" "$got"; fi

echo
echo "$PASS passed, $FAIL failed"
[ $FAIL = 0 ]
