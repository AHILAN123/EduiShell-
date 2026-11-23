/*
 * shell.h - shared definitions for EduiShell.
 *
 * Every .c file includes this header FIRST (before any system header),
 * so that the _POSIX_C_SOURCE line below is seen before <stdio.h> etc.
 */
#ifndef SHELL_H
#define SHELL_H

/*
 * We compile with -std=c11, which on its own hides everything that is not
 * plain ISO C (fork, pipe, dup2, ...). This line asks the C library to also
 * show us the POSIX functions. It must come before any #include.
 */
#define _POSIX_C_SOURCE 200809L

/* ---------- Size limits (fixed-size arrays, no malloc) ---------- */

#define MAX_LINE 1024      /* longest input line, including the '\n' */
#define MAX_ARGS 64        /* most words in one command, including the final NULL */
#define MAX_CMDS 16        /* most commands in one pipeline (a | b | c ...) */
#define MAX_PIPELINES 16   /* most pipelines on one line (separated by ; or &) */
#define MAX_TOKENS 128     /* most tokens (words and operators) on one line */
#define MAX_PATH 4096      /* longest directory path we print */
#define MAX_HISTORY 50     /* how many old lines "history" remembers */

/* ---------- Return values of read_line() ---------- */

#define READ_OK 1       /* a line was read */
#define READ_EOF 0      /* end of input (Ctrl-D) */
#define READ_SKIP -1    /* nothing usable was read, ask again */

/* ---------- Tokens: the "words" the tokenizer cuts a line into ---------- */

enum token_type {
    TOKEN_WORD,        /* an ordinary word, e.g. ls or "hello world" */
    TOKEN_PIPE,        /* |  */
    TOKEN_INPUT,       /* <  */
    TOKEN_OUTPUT,      /* >  */
    TOKEN_APPEND,      /* >> */
    TOKEN_BACKGROUND,  /* &  */
    TOKEN_SEMICOLON    /* ;  */
};

struct token {
    enum token_type type;
    char text[MAX_LINE];   /* the word with its quotes removed, or the operator */
};

struct token_list {
    struct token items[MAX_TOKENS];
    int count;
};

/* ---------- Parsed commands ---------- */

/*
 * One simple command, for example:  sort < in.txt > out.txt
 * All the char pointers point into a struct token_list, so the token list
 * must stay alive for as long as the command is used.
 */
struct command {
    char *argv[MAX_ARGS];  /* argv[0] is the program name; the list ends with NULL */
    int argc;              /* number of words in argv (not counting the NULL) */
    char *input_file;      /* file name after <, or NULL */
    char *output_file;     /* file name after > or >>, or NULL */
    int append_output;     /* 1 if the output file was given with >>, 0 for > */
};

/* Commands joined by pipes, for example:  ls | grep c | wc -l */
struct pipeline {
    struct command cmds[MAX_CMDS];
    int num_cmds;
    int background;        /* 1 if the pipeline ended with & */
};

/* A whole input line, for example:  cd /tmp ; ls | wc -l ; sleep 5 & */
struct command_line {
    struct pipeline pipelines[MAX_PIPELINES];
    int num_pipelines;
};

/* ---------- main.c ---------- */
void print_prompt(void);
int read_line(char *line, int size);

/* ---------- parser.c ---------- */
int parse_line(const char *line, struct token_list *tokens, struct command_line *cmdline);

/* ---------- builtins.c ---------- */
int is_builtin(const char *name);
int must_run_in_parent(const char *name);
int run_builtin(struct command *cmd);
void add_to_history(const char *line);

/* ---------- execute.c ---------- */
void execute_command_line(struct command_line *cmdline);
void reap_background_jobs(void);

#endif
