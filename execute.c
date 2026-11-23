/*
 * execute.c - start programs with fork + execvp and wait for them.
 */
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * report_exec_error - explain why execvp() failed.
 * Inputs: program - the name we tried to run (errno is still set by execvp).
 * Returns: nothing.
 */
static void report_exec_error(const char *program)
{
    /* ENOENT means "no such file": the program is not in any PATH folder. */
    if (errno == ENOENT) {
        fprintf(stderr, "eduishell: %s: command not found\n", program);
    } else {
        fprintf(stderr, "eduishell: %s: %s\n", program, strerror(errno));
    }
}

/*
 * redirect_input - make the file the process's standard input (fd 0).
 * Inputs: file_name - the file after <.
 * Returns: 0 on success, -1 on error (a message has been printed).
 */
static int redirect_input(const char *file_name)
{
    int fd = open(file_name, O_RDONLY);

    if (fd == -1) {
        fprintf(stderr, "eduishell: %s: %s\n", file_name, strerror(errno));
        return -1;
    }
    /*
     * dup2(fd, 0) makes fd 0 point to the same open file as fd. Programs
     * read their input from fd 0, so they now read from the file without
     * knowing it. Afterwards the extra descriptor fd is no longer needed.
     */
    if (dup2(fd, STDIN_FILENO) == -1) {
        perror("eduishell: dup2");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * redirect_output - make the file the process's standard output (fd 1).
 * Inputs: file_name - the file after > or >>;
 *         append - 1 for >> (add to the end), 0 for > (empty the file first).
 * Returns: 0 on success, -1 on error (a message has been printed).
 */
static int redirect_output(const char *file_name, int append)
{
    int fd;
    /* O_CREAT: create the file if it does not exist yet. */
    int flags = O_WRONLY | O_CREAT;

    if (append) {
        flags = flags | O_APPEND;   /* every write goes to the end of the file */
    } else {
        flags = flags | O_TRUNC;    /* cut the file to length 0 first */
    }

    /* 0644 = owner may read+write, everyone else may only read. */
    fd = open(file_name, flags, 0644);
    if (fd == -1) {
        fprintf(stderr, "eduishell: %s: %s\n", file_name, strerror(errno));
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1) {
        perror("eduishell: dup2");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * setup_redirections - apply a command's  < file  and  > file / >> file.
 * Called in the child after fork(), so the shell's own stdin and stdout
 * are never changed.
 * Inputs: cmd - the command.
 * Returns: 0 on success, -1 on error.
 */
static int setup_redirections(struct command *cmd)
{
    if (cmd->input_file != NULL) {
        if (redirect_input(cmd->input_file) == -1) {
            return -1;
        }
    }
    if (cmd->output_file != NULL) {
        if (redirect_output(cmd->output_file, cmd->append_output) == -1) {
            return -1;
        }
    }
    return 0;
}

/*
 * close_all_pipes - close both ends of every pipe in the array.
 *
 * WHY THIS MATTERS: a program reading from a pipe (e.g. "wc -l") only stops
 * when it sees end-of-file, and the kernel only reports end-of-file once
 * EVERY write end of that pipe is closed - in every process. fork() copies
 * all open descriptors into every child, so each pipe end exists in many
 * processes. If even one forgotten copy of a write end stays open (in the
 * shell or in any child), the reader waits forever and the shell hangs.
 * Unused read ends must be closed too: a writer only gets "nobody is
 * listening" (SIGPIPE) when every read end is closed, so "yes | head -1"
 * would never stop.
 *
 * Inputs: pipe_fds - the pipes; num_pipes - how many there are.
 * Returns: nothing.
 */
static void close_all_pipes(int pipe_fds[][2], int num_pipes)
{
    int i;

    for (i = 0; i < num_pipes; i++) {
        close(pipe_fds[i][0]);
        close(pipe_fds[i][1]);
    }
}

/*
 * wait_for_children - wait until every child in the list has finished.
 * Waiting is what removes a finished child from the process table;
 * a child nobody waits for stays behind as a "zombie".
 * Inputs: child_pids - process IDs of the children; count - how many.
 * Returns: nothing.
 */
static void wait_for_children(pid_t child_pids[], int count)
{
    int i;
    int status;

    for (i = 0; i < count; i++) {
        /*
         * waitpid() can return early with errno EINTR if a signal arrives
         * while we wait. That does not mean the child finished, so try again.
         */
        while (waitpid(child_pids[i], &status, 0) == -1) {
            if (errno != EINTR) {
                perror("eduishell: waitpid");
                break;
            }
        }
    }
}

/*
 * create_pipes - create the pipes that connect the commands of a pipeline.
 * A pipe is a one-way channel in the kernel: pipe_fds[i][1] is the
 * WRITE end and pipe_fds[i][0] is the READ end.
 * Inputs: pipe_fds - array to fill; num_pipes - how many pipes to create.
 * Returns: 0 on success, -1 on error (any pipes already made are closed).
 */
static int create_pipes(int pipe_fds[][2], int num_pipes)
{
    int i;

    for (i = 0; i < num_pipes; i++) {
        if (pipe(pipe_fds[i]) == -1) {
            perror("eduishell: pipe");
            close_all_pipes(pipe_fds, i);
            return -1;
        }
    }
    return 0;
}

/*
 * connect_to_pipes - in the child for command number 'index', plug its
 * stdin and stdout into the neighbouring pipes.
 *
 *   cmd 0  --pipe 0-->  cmd 1  --pipe 1-->  cmd 2
 *
 * Command i reads from pipe i-1 and writes into pipe i.
 * Inputs: index - which command this child runs; num_cmds - commands in
 *         the pipeline; pipe_fds - all the pipes.
 * Returns: nothing (the child exits if dup2 fails).
 */
static void connect_to_pipes(int index, int num_cmds, int pipe_fds[][2])
{
    /* Every command except the first reads from the pipe before it. */
    if (index > 0) {
        if (dup2(pipe_fds[index - 1][0], STDIN_FILENO) == -1) {
            perror("eduishell: dup2");
            exit(1);
        }
    }
    /* Every command except the last writes into the pipe after it. */
    if (index < num_cmds - 1) {
        if (dup2(pipe_fds[index][1], STDOUT_FILENO) == -1) {
            perror("eduishell: dup2");
            exit(1);
        }
    }
}

/*
 * run_child - everything the child process for command 'index' does.
 * Inputs: pl - the pipeline; index - which command to run; pipe_fds - pipes.
 * Returns: never - the child either becomes the program (execvp) or exits.
 */
static void run_child(struct pipeline *pl, int index, int pipe_fds[][2])
{
    struct command *cmd = &pl->cmds[index];

    /*
     * The shell catches Ctrl-C (SIGINT) so that it does not die itself
     * (see main.c), and the child inherited that handler. execvp() would
     * reset a caught signal to its default anyway, but a builtin running
     * in this child never calls execvp(), so we choose explicitly:
     *  - a foreground command should be stopped by Ctrl-C (SIG_DFL);
     *  - a background command should ignore it (SIG_IGN). Ctrl-C goes to
     *    every process started from the terminal, and the user meant to
     *    stop only the command in front of them, not jobs left running.
     *    An ignored signal stays ignored after execvp(), so this works
     *    for external programs too.
     */
    if (pl->background) {
        signal(SIGINT, SIG_IGN);
    } else {
        signal(SIGINT, SIG_DFL);
    }

    connect_to_pipes(index, pl->num_cmds, pipe_fds);

    /*
     * dup2() made COPIES of the pipe ends we need on fd 0 and fd 1, so the
     * original pipe descriptors are now all extra. Close every one of them,
     * or a write end would stay open in this process and the reader at the
     * other end would never see end-of-file (see close_all_pipes).
     */
    close_all_pipes(pipe_fds, pl->num_cmds - 1);

    /* Redirections come after the pipes, so "ls > f | wc" writes to f, like bash. */
    if (setup_redirections(cmd) == -1) {
        exit(1);
    }
    /* A builtin such as echo runs here and the child then ends. */
    if (is_builtin(cmd->argv[0])) {
        exit(run_builtin(cmd));
    }
    /*
     * execvp() replaces this process with the program. It searches the
     * folders in $PATH for argv[0]. If it returns at all, it failed.
     */
    execvp(cmd->argv[0], cmd->argv);
    report_exec_error(cmd->argv[0]);
    /* 127 is the usual "command not found" exit code. */
    exit(127);
}

/*
 * report_background - tell the user the PIDs of a background pipeline.
 * Inputs: child_pids - the process IDs; count - how many.
 * Returns: nothing.
 */
static void report_background(pid_t child_pids[], int count)
{
    int i;

    /* Like bash, job messages go to stderr so they never mix into output files. */
    for (i = 0; i < count; i++) {
        fprintf(stderr, "[background] pid %d\n", (int)child_pids[i]);
    }
}

/*
 * reap_background_jobs - collect background children that have finished.
 * waitpid(-1, ..., WNOHANG) means "any child that has already ended, but
 * do not wait if none has". Called before every prompt, so a finished
 * background job never stays a zombie for long.
 * Inputs: none.
 * Returns: nothing.
 */
void reap_background_jobs(void)
{
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG);

    /* 0 = children exist but none has ended; -1 = no children at all. */
    while (pid > 0) {
        fprintf(stderr, "[done] pid %d\n", (int)pid);
        pid = waitpid(-1, &status, WNOHANG);
    }
}

/*
 * run_pipeline - start every command of a pipeline, each in its own child
 * process, connected by pipes, then wait for all of them (unless the
 * pipeline ends with &). A single command is a pipeline with 0 pipes.
 * Inputs: pl - the pipeline.
 * Returns: nothing.
 */
static void run_pipeline(struct pipeline *pl)
{
    int pipe_fds[MAX_CMDS - 1][2];   /* n commands need n-1 pipes */
    pid_t child_pids[MAX_CMDS];
    int num_pipes = pl->num_cmds - 1;
    int num_started = 0;
    int i;
    pid_t pid;

    /* All pipes are made BEFORE forking, so every child inherits all of them. */
    if (create_pipes(pipe_fds, num_pipes) == -1) {
        return;
    }

    /*
     * fork() copies our whole process, including any text still waiting in
     * the stdout buffer. Flush first, or that text would be printed twice.
     */
    fflush(stdout);

    for (i = 0; i < pl->num_cmds; i++) {
        /* fork() returns twice: 0 in the new child, the child's PID in the parent. */
        pid = fork();
        if (pid == -1) {
            perror("eduishell: fork");
            break;
        }
        if (pid == 0) {
            run_child(pl, i, pipe_fds);
        }
        child_pids[num_started] = pid;
        num_started++;
    }

    /*
     * The parent only starts the children; it never reads or writes the
     * pipes itself. It must close ALL its pipe ends, otherwise the last
     * command would wait forever for more input (see close_all_pipes).
     */
    close_all_pipes(pipe_fds, num_pipes);

    /* With & we do not wait now; reap_background_jobs() collects them later. */
    if (pl->background) {
        report_background(child_pids, num_started);
    } else {
        wait_for_children(child_pids, num_started);
    }
}

/*
 * execute_pipeline - run one pipeline, choosing where builtins run.
 * Inputs: pl - the pipeline.
 * Returns: nothing.
 */
static void execute_pipeline(struct pipeline *pl)
{
    struct command *first = &pl->cmds[0];

    /*
     * cd and exit change the shell itself, so when they are the whole
     * pipeline they run right here in the parent. In a child they would
     * only change the child (see builtins.c). Inside a longer pipeline,
     * e.g. "cd /tmp | ls", or with &, they run in a child, like in bash.
     */
    if (pl->num_cmds == 1 && !pl->background && must_run_in_parent(first->argv[0])) {
        run_builtin(first);
        return;
    }
    run_pipeline(pl);
}

/*
 * execute_command_line - run every pipeline on one parsed input line,
 * one after another (this is what ; means). A pipeline that ended with &
 * is started and the next one begins without waiting for it.
 * Inputs: cmdline - the result of parse_line().
 * Returns: nothing.
 */
void execute_command_line(struct command_line *cmdline)
{
    int i;

    for (i = 0; i < cmdline->num_pipelines; i++) {
        execute_pipeline(&cmdline->pipelines[i]);
    }
}
