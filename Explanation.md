# How EduiShell works

This document explains the shell for someone who knows basic C but has never
written a shell. Read it next to the source code: every function mentioned
here has a comment above it in the code.

---

## 1. Code structure

| File | What it does |
|---|---|
| `shell.h` | Size limits (`MAX_LINE`, `MAX_ARGS`, `MAX_CMDS`, ...), the structs, and the prototypes of all shared functions. Every `.c` file includes it first. |
| `main.c` | The main loop: show prompt → read a line → parse it → execute it. Also the Ctrl-C handler. |
| `parser.c` | Turns a line of text into structs: first cuts it into *tokens*, then groups the tokens into commands and pipelines. |
| `execute.c` | Runs the parsed commands: `fork`, `execvp`, `waitpid`, pipes, redirection, background jobs. |
| `builtins.c` | The commands that are part of the shell itself: `cd`, `pwd`, `exit`, `echo`, `help`, `history`. |
| `tests/run_tests.sh` | Runs each test through bash and through our shell and compares the output. |

### The data structures

A line is parsed into arrays inside arrays (no linked lists, no trees):

```
struct command_line              the whole line
 └─ pipelines[]                  split at ; and &
     └─ struct pipeline
         ├─ background           1 if it ended with &
         └─ cmds[]               split at |
             └─ struct command
                 ├─ argv[]       the words, ending with NULL (what execvp wants)
                 ├─ input_file   after <   (or NULL)
                 ├─ output_file  after > or >>  (or NULL)
                 └─ append_output  1 for >>, 0 for >
```

For example `cat < in.txt | sort ; echo done &` becomes:

```
pipelines[0]  background = 0
    cmds[0]  argv = {"cat", NULL}           input_file = "in.txt"
    cmds[1]  argv = {"sort", NULL}
pipelines[1]  background = 1
    cmds[0]  argv = {"echo", "done", NULL}
```

The `argv` strings are not copied. They point into the token list, which
lives in `main()` for the whole loop iteration. That's why the shell needs
no `malloc` at all, and therefore can't leak memory.

### Who calls whom

```
main
 ├─ setup_signals                      install the Ctrl-C handler
 └─ loop:
     ├─ reap_background_jobs           collect finished & jobs (no zombies)
     ├─ print_prompt                   only if stdin is a terminal
     ├─ read_line                      fgets one line
     ├─ add_to_history
     ├─ parse_line
     │    ├─ tokenize
     │    │    ├─ read_operator        | < > >> & ;
     │    │    └─ read_word            quotes, $VARIABLES
     │    │         ├─ expand_variable (getenv)
     │    │         └─ append_char
     │    └─ parse_tokens              split at ; and &
     │         └─ parse_pipeline       split at |
     │              └─ parse_command   words -> argv, < > >> -> file names
     └─ execute_command_line           every pipeline, one after another
          └─ execute_pipeline
               ├─ run_builtin          cd / exit: run in the shell itself
               └─ run_pipeline         everything else
                    ├─ create_pipes
                    ├─ fork (one child per command)
                    │    └─ run_child                     (in the child)
                    │         ├─ connect_to_pipes         dup2 onto stdin/stdout
                    │         ├─ close_all_pipes
                    │         ├─ setup_redirections       open + dup2
                    │         └─ run_builtin  or  execvp
                    ├─ close_all_pipes                    (in the parent)
                    └─ wait_for_children  or  report_background (for &)
```

---

## 2. The main loop: read → parse → execute

`main()` in `main.c` repeats these steps until the input ends:

1. **Reap background jobs.** `reap_background_jobs()` collects any `&` job
   that has finished, so it doesn't stay a zombie (see section 3).
2. **Prompt.** If stdin is a terminal (`isatty`), print the current
   directory (`getcwd`) and `$ `, then `fflush(stdout)`. The prompt has no
   `\n`, and a terminal only sends text on a newline unless we flush. When
   input comes from a pipe or a file (as in the tests), no prompt is
   printed, just like bash.
3. **Read.** `read_line()` uses `fgets()` to read one line and removes its
   `\n`. It reports three cases:
   - `READ_OK`: a line was read.
   - `READ_EOF`: the input ended (Ctrl-D), so the loop stops and the shell exits.
   - `READ_SKIP`: Ctrl-C was pressed or the line was too long, so go round again.
4. **Remember** the line for `history` (blank lines are skipped).
5. **Parse.** `parse_line()` fills the structs above. On a syntax error it
   prints a message and the line is not run at all.
6. **Execute.** `execute_command_line()` runs each pipeline in turn.

### How parsing works

**Step 1: tokenize.** The line is read one character at a time:

```
echo "a b">$HOME/f|wc     ->     [echo] [a b] [>] [/home/me/f] [|] [wc]
```

- Spaces separate words, except inside quotes.
- `| < > >> & ;` are always tokens of their own, even with no spaces around
  them. `>` followed by `>` becomes one `>>` token.
- Quotes are removed. Inside `'...'` or `"..."`, spaces and operators are
  ordinary characters.
- `$NAME` is replaced with `getenv("NAME")`, both outside quotes and inside
  `"..."`. Inside `'...'` it is left alone, as in bash.
- Each token remembers its *type* (`TOKEN_WORD`, `TOKEN_PIPE`, ...), so
  `"|"` in quotes is a word, while a bare `|` is a pipe.

**Step 2: group.** `parse_tokens()` cuts the token list at every `;` and
`&` into pipelines. `parse_pipeline()` cuts each of those at every `|` into
commands. `parse_command()` puts words into `argv` and takes the word after
`<`, `>` or `>>` as a file name. Every syntax error ends up as "an empty
piece":

| Input | What is empty | Message |
|---|---|---|
| `ls \|` | the command after `\|` | `syntax error near end of line` |
| `\| ls` | the command before `\|` | `syntax error near '\|'` |
| `echo a ; ; echo b` | the pipeline between the `;`s | `syntax error near ';'` |
| `ls >` | no file name after `>` | `syntax error near end of line` |
| `> f` | a command with no words | `syntax error: redirection without a command` |

---

## 3. How a command runs: fork, execvp, waitpid

A shell doesn't run programs itself. It asks the kernel to make a **new
process** for each one:

- **`fork()`** makes an almost exact copy of the calling process: the same
  code, the same variables, the same open files. It returns **twice**: it
  returns `0` in the new *child*, and the child's process ID (PID) in the
  *parent*. That return value is how each copy knows which one it is.
- **`execvp(argv[0], argv)`** *replaces* the program running in the current
  process with another one (for example `/usr/bin/ls`). It searches the
  folders listed in `$PATH`. If it succeeds it never returns, because the
  old code is gone. If it returns, it failed (for example
  `command not found`), so the child prints an error and exits with 127.
- **`waitpid(pid, &status, 0)`** makes the parent sleep until that child
  has finished. It also removes the finished child from the kernel's
  process table. A child that has ended but hasn't been waited for is a
  **zombie**.

```
        eduishell (parent, PID 100)
                 │
                 │ fork()
        ┌────────┴──────────────────────────┐
        │ returns 101                       │ returns 0
        ▼                                   ▼
  parent: waitpid(101) ...           child (PID 101):
  (sleeps)                           set up pipes / redirections
        .                            execvp("ls", argv)
        .                                   │
        .                                   ▼
        .                            now running /usr/bin/ls
        .                            ... prints, then exits
        ◄───────── child finished ──────────┘
  waitpid returns, show the next prompt
```

Two details in `run_pipeline()`:

- **`fflush(stdout)` before `fork()`.** `printf` keeps text in a buffer
  inside the process. `fork` copies that buffer, so without the flush,
  text printed by the shell earlier could come out twice.
- **Background jobs (`&`).** The parent does not call `waitpid` straight
  away. It prints the PIDs (`[background] pid 101`) and goes on.
  Before every prompt, `reap_background_jobs()` calls
  `waitpid(-1, &status, WNOHANG)`. That means "collect *any* child that
  has already finished, but don't wait if none has". So every finished
  job is reaped and no zombies are left.

---

## 4. How pipelines work

### What a pipe is

`pipe(fds)` asks the kernel for a one-way channel with two ends, which are
two file descriptors:

```
     write end                      read end
     fds[1]  ═════ kernel buffer ═════>  fds[0]
```

Whatever is written into `fds[1]` can be read from `fds[0]`. When the
buffer is empty, a reader waits. The reader gets **end-of-file** only when
**every** copy of the write end, in every process, has been closed.

Programs like `ls` and `wc` don't know about pipes. They just write to file
descriptor **1** (stdout) and read from **0** (stdin). The shell's job is
to make fd 1 of one program and fd 0 of the next point at the two ends of
a pipe. It does that with **`dup2(old_fd, new_fd)`**, which makes `new_fd`
refer to the same open file or pipe as `old_fd`. If `new_fd` was already
open, it is closed first.

### Worked example: `ls | grep c | wc -l`

There are **3 commands**, so the shell needs **2 pipes** and **3 children**.
In `run_pipeline()`:

**1. Create the pipes first** (`create_pipes`), before any fork, so every
child inherits all of them. Say the kernel hands out these numbers:

```
pipe 0:  pipe_fds[0][0] = 3 (read)    pipe_fds[0][1] = 4 (write)
pipe 1:  pipe_fds[1][0] = 5 (read)    pipe_fds[1][1] = 6 (write)
```

**2. Fork three children.** Right after `fork()`, every child has fds
0, 1, 2 (the terminal) **plus all of 3, 4, 5, 6**. Each child then runs
`run_child()`:

| Child | Command | `dup2` calls (`connect_to_pipes`) | Then closes | Ends up with |
|---|---|---|---|---|
| 0 | `ls` | `dup2(4, 1)`: stdout → pipe 0 write | 3, 4, 5, 6 | stdin = terminal, stdout = pipe 0 |
| 1 | `grep c` | `dup2(3, 0)`: stdin ← pipe 0 read<br>`dup2(6, 1)`: stdout → pipe 1 write | 3, 4, 5, 6 | stdin = pipe 0, stdout = pipe 1 |
| 2 | `wc -l` | `dup2(5, 0)`: stdin ← pipe 1 read | 3, 4, 5, 6 | stdin = pipe 1, stdout = terminal |

The rule in code: command `i` reads from pipe `i-1` (unless it's the first)
and writes into pipe `i` (unless it's the last). After the `dup2` calls,
the original descriptors 3–6 are only extra copies, so the child closes
**all** of them (`close_all_pipes`) and then calls `execvp`.

**3. The parent closes all of 3, 4, 5, 6** too. It never reads or writes
the pipes itself. Then it waits for all three children.

### The data flow

```
 ┌──────────┐  fd 1        pipe 0        fd 0  ┌──────────┐  fd 1        pipe 1        fd 0  ┌──────────┐
 │    ls    │ ───────> [ 4 ══════> 3 ] ──────> │  grep c  │ ───────> [ 6 ══════> 5 ] ──────> │  wc -l   │ ──> terminal
 │ child 0  │                                  │ child 1  │                                  │ child 2  │     (fd 1)
 └──────────┘                                  └──────────┘                                  └──────────┘

 eduishell (parent): has closed 3, 4, 5, 6 and is in waitpid() for all three children
```

All three programs run **at the same time**. `grep` starts working on the
first lines while `ls` is still producing more. When `ls` exits, the last
write end of pipe 0 closes. `grep` reads end-of-file, finishes, and exits,
which closes the last write end of pipe 1. Then `wc` sees end-of-file,
prints the count, and exits.

### Why forgetting to close a pipe end makes the shell hang

`fork` copies *every* open descriptor, so after forking, the write end of
pipe 0 (fd 4) exists in **four** processes: the parent and all three
children. `grep` gets end-of-file on pipe 0 only when **all four** copies
are closed.

Suppose the parent forgot to close fd 4. Then:

1. `ls` finishes and exits, which closes *its* copy of fd 4.
2. `grep` reads all the data, then waits for more, because a write end
   is still open (in the parent), so more data could still arrive.
3. `wc` waits for `grep`, and the parent waits in `waitpid()` for `grep`.
4. Nobody can move: the parent is waiting for `grep`, and `grep` is waiting
   for the parent to close fd 4. **The shell hangs forever.**

The same thing happens if a *child* keeps an extra write end. For example,
if `wc` kept fd 6 open, it would be waiting for end-of-file on a pipe it
could still write to itself.

Unused **read** ends matter too. In `yes | head -1`, `head` exits after one
line. The kernel stops `yes` (with SIGPIPE) only when *no process* has
the read end open any more. A forgotten read end means `yes` runs forever.

This was checked: deleting the parent's `close_all_pipes()` call made every
pipeline test in `make test` fail with "hung (exit code 124)".

### Pipelines with redirection

`sort < in.txt | uniq > out.txt` works because `run_child()` first connects
the pipes, then applies redirections. For `sort`, stdin comes from the file
and stdout goes into the pipe. For `uniq`, stdin comes from the pipe and
stdout goes to the file. If both a pipe and a file want the same fd, the
file wins because it comes second, which is the same as bash.

---

## 5. How redirection works: open + dup2

Redirection uses the same trick as pipes: put a file on fd 0 or fd 1
before `execvp`, and the program uses it without knowing.

`echo hi > out.txt` in the child (`redirect_output`):

```c
fd = open("out.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);  /* say fd = 3 */
dup2(fd, STDOUT_FILENO);                                     /* fd 1 -> out.txt */
close(fd);                                                   /* 3 is no longer needed */
/* ... execvp / builtin: everything printed to stdout lands in out.txt */
```

```
 before:   fd 1 ──> terminal          fd 3 ──> out.txt
 dup2(3,1): fd 1 ──> out.txt          fd 3 ──> out.txt
 close(3):  fd 1 ──> out.txt
```

- `>`  uses `O_TRUNC`: the file is emptied first.
- `>>` uses `O_APPEND`: every write goes to the end of the file.
- `O_CREAT` with mode `0644` creates a missing file as `rw-r--r--`.
- `<` opens the file with `O_RDONLY` and uses `dup2(fd, STDIN_FILENO)`.
- If `open` fails (missing file, missing folder, no permission), the child
  prints `eduishell: file: reason` and exits without running the command.

All of this happens in the **child**, after `fork`. The shell's own
stdin and stdout are never changed, so the next prompt still goes to the
terminal.

---

## 6. Why `cd` must be a builtin

Every process has **its own** current directory, and `chdir()` changes
only the directory of the process that calls it.

If `cd` were a normal program, the shell would `fork` a child, the child
would `chdir("/tmp")` and exit, and its directory would disappear with
it. The shell (the parent) would still be where it was:

```
 shell (in /home/me) ── fork ──> child: chdir("/tmp"), exit
 shell: still in /home/me        ✗ nothing changed
```

So `execute_pipeline()` runs `cd` and `exit` **directly in the shell
process**, without forking (`must_run_in_parent()`). `exit` is the same:
a child calling `exit()` ends only the child. After `cd`, the shell also
updates `$PWD` with `setenv`, so programs started later see the new
directory.

The other builtins (`echo`, `pwd`, `help`, `history`) don't change the
shell, so they run in a forked child like any program. That way
`echo hi > f.txt` and `history | grep ls` work with exactly the same pipe
and redirection code. When `cd` is part of a pipeline (`cd / | cat`) or
runs in the background (`cd / &`), it also runs in a child and has no
effect. bash behaves the same way.

---

## 7. Ctrl-C

Pressing Ctrl-C makes the terminal send **SIGINT** to every process in the
foreground: the shell *and* the command it is running.

- The **shell** installs a handler with `sigaction` (`setup_signals()` in
  `main.c`), so SIGINT no longer kills it. The handler only prints a newline.
- A **foreground child** sets SIGINT back to the default (`SIG_DFL`) in
  `run_child()`, so Ctrl-C kills it.
- A **background child** sets SIGINT to ignored (`SIG_IGN`), so Ctrl-C
  doesn't kill jobs the user left running.
- Ctrl-C at the prompt interrupts `fgets()` (errno `EINTR`). `read_line()`
  returns `READ_SKIP` and a fresh prompt is shown. `waitpid` can also be
  interrupted with `EINTR`; the shell then simply calls it again.

---

## 8. Problems found while building it (good viva material)

- **Text printed twice.** `fork` copies unflushed `printf` buffers, so the
  shell calls `fflush(stdout)` before every `fork`.
- **Lines read again and again** when running `./eduishell < script.txt`.
  `fgets` read 4 KB of the file ahead into a buffer. The shell and its
  children share one read position in the file, and a child exiting moved
  that position back, so the shell read the same lines again forever.
  Making stdin unbuffered (`setvbuf(stdin, NULL, _IONBF, 0)`) keeps the
  position exactly after the current line. `make valgrind` found this
  bug, and the tests now feed input from a file too.
- **A test that couldn't fail.** The first Ctrl-C test passed even with
  the SIGINT handler removed. bash starts `&` jobs in a script with SIGINT
  ignored, so the shell under test was immune anyway. The test now uses
  `set -m` and was checked to fail when the handler is missing.

---

## 9. Known limitations compared to bash

- No job control (`fg`, `bg`, `jobs`, Ctrl-Z). Background jobs are in the
  same process group, which is why they must ignore SIGINT.
- Missing syntax: `2>`, `2>&1`, here-documents `<<`, `&&`, `||`,
  `( ... )`, globbing `*.c`, `~`, `$(...)`, backslash escapes.
- No `X=value` assignments, and no `$?`, `$$` or `${NAME}`.
- A `$VARIABLE` is never split into several words.
- Only the last `<` and the last `>`/`>>` of a command are used.
- `cd` and `exit` ignore redirections.
- After a syntax error the shell carries on with the next line. A bash
  script would stop.
- Fixed limits: 1023 characters per line, 63 arguments, 16 commands per
  pipeline, 16 pipelines per line, 128 tokens per line, 50 history entries.
