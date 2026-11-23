/*
 * main.c - the heart of the shell: the read -> parse -> execute loop.
 */
#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * handle_sigint - runs when the user presses Ctrl-C.
 * Pressing Ctrl-C sends SIGINT to the shell AND to the command it is
 * running. Without this handler the default action would kill the shell
 * too. With it, the shell just moves to a new line and carries on, while
 * the command (which has the default action again, see execute.c) dies.
 * Inputs: signal_number - always SIGINT here (unused).
 * Returns: nothing.
 */
static void handle_sigint(int signal_number)
{
    (void)signal_number;
    /* Only a few functions are safe inside a signal handler. write() is one; printf() is not. */
    write(STDOUT_FILENO, "\n", 1);
}

/*
 * setup_signals - install handle_sigint() for SIGINT (Ctrl-C).
 * Inputs: none.
 * Returns: nothing.
 */
static void setup_signals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    /*
     * sa_flags = 0 (no SA_RESTART): if Ctrl-C arrives while fgets() is
     * waiting for input, fgets() gives up with errno EINTR. read_line()
     * notices that, and main() prints a fresh prompt.
     */
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) == -1) {
        perror("eduishell: sigaction");
    }
}

/*
 * print_prompt - print the current directory followed by "$ ".
 * Inputs: none.
 * Returns: nothing.
 */
void print_prompt(void)
{
    char cwd[MAX_PATH];

    /* getcwd() asks the kernel for the full path of our current directory. */
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("eduishell: getcwd");
        strcpy(cwd, "?");
    }
    printf("%s$ ", cwd);

    /*
     * stdout is line-buffered on a terminal: text is only sent when a '\n'
     * is printed. The prompt has no '\n', so we flush it by hand, otherwise
     * the user would not see it.
     */
    fflush(stdout);
}

/*
 * discard_rest_of_line - throw away input characters up to the next '\n'.
 * Used when a line was longer than our buffer.
 * Inputs: none.
 * Returns: nothing.
 */
static void discard_rest_of_line(void)
{
    int ch = getchar();

    while (ch != '\n' && ch != EOF) {
        ch = getchar();
    }
}

/*
 * read_line - read one line from standard input and remove its '\n'.
 * Inputs: line - buffer to fill; size - size of that buffer.
 * Returns: READ_OK if a line was read, READ_EOF at end of input,
 *          READ_SKIP if Ctrl-C was pressed or the line was too long.
 */
int read_line(char *line, int size)
{
    int length;

    /* fgets() reads at most size-1 characters and always adds a '\0'. */
    if (fgets(line, size, stdin) == NULL) {
        /* Interrupted by Ctrl-C: not the end of input, just ask again. */
        if (ferror(stdin) && errno == EINTR) {
            clearerr(stdin);
            return READ_SKIP;
        }
        return READ_EOF;
    }

    length = strlen(line);
    if (length > 0 && line[length - 1] == '\n') {
        line[length - 1] = '\0';
        return READ_OK;
    }

    /* No '\n': either the very last line of input, or the line was too long. */
    if (feof(stdin)) {
        return READ_OK;
    }
    discard_rest_of_line();
    fprintf(stderr, "eduishell: line too long (max %d characters)\n", size - 2);
    return READ_SKIP;
}

/*
 * main - run the shell loop until end of input.
 * Inputs: none (commands are read from standard input).
 * Returns: 0 when the user ends the input with Ctrl-D.
 */
int main(void)
{
    char line[MAX_LINE];
    struct token_list tokens;
    struct command_line cmdline;
    int result;

    /*
     * isatty() tells us whether stdin is a keyboard (terminal). We only show
     * a prompt for a human; when commands come from a pipe or file (as in our
     * tests) a prompt would just mix into the output. bash does the same.
     */
    int interactive = isatty(STDIN_FILENO);

    /*
     * Make stdin unbuffered, so fgets() never reads ahead of the current line.
     * Why: when commands come from a file (./eduishell < script.txt), the
     * shell and its children share ONE read position in that file. With a
     * buffer, the shell would grab a big chunk of the file at once; a child
     * that exits would then move the shared position back to "its" place,
     * and the shell would read the same lines again - forever. Reading
     * without a buffer keeps the position exactly after the current line,
     * so a command such as "head -n 1" also gets the next line, as in bash.
     */
    setvbuf(stdin, NULL, _IONBF, 0);

    setup_signals();
    while (1) {
        /* Collect finished background jobs so they do not stay zombies. */
        reap_background_jobs();
        if (interactive) {
            print_prompt();
        }

        result = read_line(line, sizeof(line));
        if (result == READ_EOF) {
            break;
        }
        if (result == READ_SKIP) {
            continue;
        }

        add_to_history(line);
        if (parse_line(line, &tokens, &cmdline) == 0) {
            execute_command_line(&cmdline);
        }
    }

    /* After Ctrl-D the cursor is still on the prompt line; move to a new line. */
    if (interactive) {
        printf("\n");
    }
    return 0;
}
