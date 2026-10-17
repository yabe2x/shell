CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -g
SAN     = -std=c11 -Wall -Wextra -O0 -g -fsanitize=address,undefined

SRC = src/main.c src/lexer.c src/parser.c src/exec.c src/jobs.c src/builtins.c
HDR = src/shell.h

.PHONY: all test clean

all: ysh

ysh: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

ysh-asan: $(SRC) $(HDR)
	$(CC) $(SAN) -o $@ $(SRC)

test: ysh ysh-asan
	./tests/run_tests.sh ./ysh
	./tests/run_tests.sh ./ysh-asan
	python3 tests/test_interactive.py ./ysh

clean:
	rm -f ysh ysh-asan
