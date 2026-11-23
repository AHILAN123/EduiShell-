/*
 * builtins.c - commands that are part of the shell itself:
 *              cd, pwd, exit, echo, help, history.
 *
 * WHY SOME COMMANDS MUST BE BUILT IN
 * Every process has its own current directory, and a process can only
 * change ITS OWN directory with chdir(). If "cd" were a separate program,
 * the shell would fork a child, the child would chdir() and then exit, and
 * the shell (the parent) would still be in the old directory - nothing
 * would happen. The same goes for "exit": a child calling exit() only ends
 * the child. So cd and exit must run inside the shell process itself.
 *
 * echo, pwd, help and history do not change the shell, so we run them in a
 * forked child like any other program (see execute.c). That way redirection
 * and pipes work for them with exactly the same code: echo hi > f.txt
 */
#include "shell.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * The last MAX_HISTORY lines the user typed, oldest first.
 * history_total counts every line ever added, so that the numbers printed
 * by "history" keep growing like in bash even after old lines are dropped.
 */
static char history_lines[MAX_HISTORY][MAX_LINE];
static int history_count = 0;
static int history_total = 0;

/*
 * is_blank - check whether a line contains only spaces and tabs.
 * Inputs: line - the text.
 * Returns: 1 if blank (or empty), 0 otherwise.
 */
static int is_blank(const char *line)
{
    int i;

    for (i = 0; line[i] != '\0'; i++) {
        if (line[i] != ' ' && line[i] != '\t') {
            return 0;
        }
    }
    return 1;
}

/*
 * add_to_history - remember a line the user typed (blank lines are skipped).
 * Inputs: line - the text of the line.
 * Returns: nothing.
 */
void add_to_history(const char *line)
{
    int i;

    if (is_blank(line)) {
        return;
    }
    /* When full, forget the oldest line by moving every line up one place. */
    if (history_count == MAX_HISTORY) {
        for (i = 1; i < MAX_HISTORY; i++) {
            strcpy(history_lines[i - 1], history_lines[i]);
        }
        history_count--;
    }
    strcpy(history_lines[history_count], line);
    history_count++;
    history_total++;
}

/*
 * builtin_history - print the remembered lines with their numbers.
 * Inputs: none.
 * Returns: 0 (success).
 */
static int builtin_history(void)
{
    int i;
    int first_number = history_total - history_count + 1;

    for (i = 0; i < history_count; i++) {
        printf("%5d  %s\n", first_number + i, history_lines[i]);
    }
    return 0;
}

/*
 * builtin_cd - change the shell's current directory.
 * "cd" alone goes to $HOME, "cd DIR" goes to DIR.
 * Inputs: cmd - the command (argv[1] is the directory, if given).
 * Returns: 0 on success, 1 on error.
 */
static int builtin_cd(struct command *cmd)
{
    const char *target;
    char new_dir[MAX_PATH];

    if (cmd->argc > 2) {
        fprintf(stderr, "eduishell: cd: too many arguments\n");
        return 1;
    }
    if (cmd->argc == 1) {
        target = getenv("HOME");
        if (target == NULL) {
            fprintf(stderr, "eduishell: cd: HOME is not set\n");
            return 1;
        }
    } else {
        target = cmd->argv[1];
    }

    if (chdir(target) == -1) {
        fprintf(stderr, "eduishell: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    /* Keep $PWD up to date like bash does; programs we start may read it. */
    if (getcwd(new_dir, sizeof(new_dir)) != NULL) {
        setenv("PWD", new_dir, 1);
    }
    return 0;
}

/*
 * builtin_pwd - print the current directory.
 * Inputs: none.
 * Returns: 0 on success, 1 on error.
 */
static int builtin_pwd(void)
{
    char cwd[MAX_PATH];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("eduishell: pwd");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}

/*
 * builtin_exit - end the shell. "exit" uses status 0, "exit N" uses N.
 * Inputs: cmd - the command (argv[1] is the status, if given).
 * Returns: does not return.
 */
static int builtin_exit(struct command *cmd)
{
    int status = 0;

    if (cmd->argc > 1) {
        status = atoi(cmd->argv[1]);
    }
    /* exit() also flushes stdout, so nothing we printed is lost. */
    exit(status);
}

/*
 * builtin_echo - print the arguments separated by spaces.
 * "echo -n ..." leaves out the newline at the end.
 * Inputs: cmd - the command.
 * Returns: 0 (success).
 */
static int builtin_echo(struct command *cmd)
{
    int i;
    int first = 1;
    int print_newline = 1;

    if (cmd->argc > 1 && strcmp(cmd->argv[1], "-n") == 0) {
        print_newline = 0;
        first = 2;
    }
    for (i = first; i < cmd->argc; i++) {
        if (i > first) {
            printf(" ");
        }
        printf("%s", cmd->argv[i]);
    }
    if (print_newline) {
        printf("\n");
    }
    return 0;
}

/*
 * builtin_help - print a short list of what the shell can do.
 * Inputs: none.
 * Returns: 0 (success).
 */
static int builtin_help(void)
{
    printf("EduiShell - a small teaching shell\n");
    printf("\n");
    printf("Built-in commands:\n");
    printf("  cd [dir]      change directory (no dir: go to $HOME)\n");
    printf("  pwd           print the current directory\n");
    printf("  echo [-n] ... print the arguments\n");
    printf("  history       show the last %d commands\n", MAX_HISTORY);
    printf("  help          show this text\n");
    printf("  exit [n]      leave the shell with status n\n");
    printf("\n");
    printf("Any other word is run as a program found in $PATH.\n");
    return 0;
}

/*
 * is_builtin - check whether a command name is one of our builtins.
 * Inputs: name - the command name (argv[0]).
 * Returns: 1 if it is a builtin, 0 otherwise.
 */
int is_builtin(const char *name)
{
    return strcmp(name, "cd") == 0
        || strcmp(name, "pwd") == 0
        || strcmp(name, "exit") == 0
        || strcmp(name, "echo") == 0
        || strcmp(name, "help") == 0
        || strcmp(name, "history") == 0;
}

/*
 * must_run_in_parent - check whether a builtin changes the shell itself
 * and therefore must NOT run in a child (see the comment at the top).
 * Inputs: name - the command name (argv[0]).
 * Returns: 1 for cd and exit, 0 otherwise.
 */
int must_run_in_parent(const char *name)
{
    return strcmp(name, "cd") == 0 || strcmp(name, "exit") == 0;
}

/*
 * run_builtin - run a builtin command.
 * Inputs: cmd - the command; cmd->argv[0] must be a builtin name.
 * Returns: the builtin's exit status (0 = success).
 */
int run_builtin(struct command *cmd)
{
    const char *name = cmd->argv[0];

    if (strcmp(name, "cd") == 0) {
        return builtin_cd(cmd);
    } else if (strcmp(name, "pwd") == 0) {
        return builtin_pwd();
    } else if (strcmp(name, "exit") == 0) {
        return builtin_exit(cmd);
    } else if (strcmp(name, "echo") == 0) {
        return builtin_echo(cmd);
    } else if (strcmp(name, "help") == 0) {
        return builtin_help();
    } else {
        return builtin_history();
    }
}
