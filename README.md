# EduiShell

A small POSIX-style Unix shell written in plain C11 for a university mini
project. It supports pipelines, redirection, builtins, background jobs and
environment variables, and is written to be read: simple C, fixed-size
arrays, no `malloc`, and a comment explaining every system call.

For a beginner-friendly walk through the code (with diagrams of how pipes
and redirection work) see **[Explanation.md](Explanation.md)**.

## Build and run

Target: Linux Mint / Ubuntu (x86_64), GCC, GNU Make.

```sh
make            # builds ./eduishell
./eduishell     # start the shell; leave with Ctrl-D or "exit"
make test       # compare our output with bash on 100+ test cases
make valgrind   # run a batch of commands under valgrind (leak check)
make clean      # remove the build output
```

`make valgrind` needs valgrind installed (`sudo apt install valgrind`).

You can also run a file of commands: `./eduishell < script.txt`.

## Features

| Feature | Example |
|---|---|
| Prompt shows the current directory | `/home/me/project$ ` |
| External programs (searched in `$PATH`) | `ls -l /tmp` |
| Double and single quotes | `echo "hello   world" 'a | b'` |
| Operators work without spaces | `echo hi>out.txt;cat<out.txt` |
| Builtins | `cd` (no argument → `$HOME`), `pwd`, `echo [-n]`, `exit [n]`, `help`, `history` (last 50 lines) |
| Input / output redirection | `sort < in.txt > out.txt`, `echo more >> out.txt` |
| Pipelines (up to 16 commands) | `cat /etc/passwd \| cut -d: -f1 \| sort \| uniq -c \| head` |
| Pipelines + redirection | `sort < in.txt \| uniq > out.txt` |
| Several commands on one line | `cd /tmp; pwd` |
| Background jobs (PID printed, reaped later, no zombies) | `sleep 10 &` |
| Environment variables | `echo $HOME "$USER" '$NOT_EXPANDED'` |
| Ctrl-C stops the running command, not the shell | `sleep 100` then Ctrl-C |
| Clear syntax errors | `ls \|` → `eduishell: syntax error near end of line` |

New files are created with permission `0644`. Error messages go to stderr.
Background job messages (`[background] pid 123`, `[done] pid 123`) also go
to stderr, so they never end up inside output files.

## Files

```
Makefile              all, test, valgrind, clean
shell.h               size limits, structs, function prototypes
main.c                read -> parse -> execute loop, prompt, Ctrl-C handler
parser.c              tokenizer (quotes, operators, $VARS) and grouping into commands
execute.c             fork/execvp/waitpid, pipes, redirection, background jobs
builtins.c            cd, pwd, exit, echo, help, history
tests/run_tests.sh    runs every test through bash and eduishell and compares
tests/valgrind_input.txt   commands used by "make valgrind"
```

## Testing

`tests/run_tests.sh` feeds each test case to **bash** and to **eduishell**
through stdin, each run in the same freshly emptied directory, and compares
what they print on **stdout**. It prints `PASS`/`FAIL` for every case and a
total at the end. Error messages (stderr) are not compared, because they
are worded differently from bash's. For error cases the script checks that
eduishell printed *some* message on stderr instead.

A few features cannot be compared with a non-interactive bash (for example
`history`, which is switched off there). Those cases are checked against a
fixed expected output instead. Ctrl-C is tested by really sending SIGINT to
the shell's process group while it runs `sleep 3`. A few cases also feed
the input from a regular file instead of a pipe.

Every test has a 5-second limit, so a hang (for example, a pipe end that was
not closed) shows up as `FAIL (hung or crashed, exit code 124)` instead of
freezing the run.

The tests and valgrind were run in an Ubuntu 24.04 container (GCC 13.3,
bash 5.2, valgrind 3.22; arm64, since the development machine was an Apple
Silicon Mac; the code uses only portable POSIX calls, so x86_64 Mint
behaves the same). Results: **103 / 103 tests pass** and valgrind reports
**0 errors and "All heap blocks were freed"** for the shell and for every
child that runs a builtin.

## Known limitations (compared to bash)

- No job control: there is no `fg`, `bg`, `jobs`, and no Ctrl-Z.
- No `2>`, `2>&1`, `<<` (here-documents), `&&`, `||`, subshells `( ... )`,
  globbing (`*.c`), `~`, or command substitution `$(...)`.
- No backslash escapes (`\"`, `\ `); use quotes instead.
- No variable assignment (`X=5`) and no `$?`, `$$` or `${NAME}`.
- A `$VARIABLE` is never split into several words. `X="a b"` would stay
  one argument (bash would split it).
- Only the last `<` and the last `>`/`>>` of a command are used. Earlier
  ones are ignored and not created.
- `cd` and `exit` ignore redirections (`cd /tmp > f` does not create `f`).
- A syntax error cancels the whole line, but the shell keeps reading the
  next line. A non-interactive bash would stop the whole script instead.
- Limits: 1023 characters per line, 63 arguments per command, 16 commands
  per pipeline, 16 pipelines per line, 128 tokens per line.
