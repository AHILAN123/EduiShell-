/*
 * parser.c - turn a line of text into commands the executor can run.
 *
 * This happens in two steps:
 *
 *   1. tokenize():  cut the line into tokens (words and operators),
 *      removing quotes and replacing $NAME with the variable's value.
 *        echo "a b">$HOME/f | wc  ->  [echo] [a b] [>] [/home/me/f] [|] [wc]
 *
 *   2. parse_tokens():  group the tokens into a struct command_line:
 *        - split on ; and &  into pipelines,
 *        - split each pipeline on |  into commands,
 *        - inside a command, words go into argv and  < > >>  set file names.
 *
 * No tree (AST) is built: the result is just arrays inside arrays.
 */
#include "shell.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return values of read_word() */
#define WORD_OK 0        /* a word was read */
#define WORD_VANISHED 1  /* the word was only an empty $VARIABLE, so there is no word */
#define WORD_ERROR -1    /* a syntax error; a message has been printed */

/* ==================== Step 1: tokenizer ==================== */

/*
 * is_space - check whether a character separates words.
 * Inputs: ch - the character.
 * Returns: 1 for a space or tab, 0 otherwise.
 */
static int is_space(char ch)
{
    return ch == ' ' || ch == '\t';
}

/*
 * is_operator_char - check whether a character starts an operator.
 * Inputs: ch - the character.
 * Returns: 1 for | < > & ; and 0 otherwise.
 */
static int is_operator_char(char ch)
{
    return ch == '|' || ch == '<' || ch == '>' || ch == '&' || ch == ';';
}

/*
 * read_operator - read one operator token starting at line[*pos].
 * Inputs: line - the input; pos - index to start at (moved past the operator);
 *         tok - the token to fill.
 * Returns: nothing (the caller already checked that line[*pos] is an operator).
 */
static void read_operator(const char *line, int *pos, struct token *tok)
{
    char ch = line[*pos];

    /* ">>" is the only operator that is two characters long. */
    if (ch == '>' && line[*pos + 1] == '>') {
        tok->type = TOKEN_APPEND;
        strcpy(tok->text, ">>");
        *pos = *pos + 2;
        return;
    }

    switch (ch) {
    case '|':
        tok->type = TOKEN_PIPE;
        break;
    case '<':
        tok->type = TOKEN_INPUT;
        break;
    case '>':
        tok->type = TOKEN_OUTPUT;
        break;
    case '&':
        tok->type = TOKEN_BACKGROUND;
        break;
    default:
        tok->type = TOKEN_SEMICOLON;
        break;
    }
    tok->text[0] = ch;
    tok->text[1] = '\0';
    *pos = *pos + 1;
}

/*
 * append_char - add one character to the end of the word being built.
 * Inputs: word - the word; length - its current length (increased by 1);
 *         ch - the character to add.
 * Returns: 0 on success, -1 if the word would be longer than MAX_LINE - 1.
 */
static int append_char(char *word, int *length, char ch)
{
    if (*length == MAX_LINE - 1) {
        fprintf(stderr, "eduishell: word too long\n");
        return -1;
    }
    word[*length] = ch;
    *length = *length + 1;
    return 0;
}

/*
 * is_name_char - check whether a character can be part of a variable name.
 * Names are made of letters, digits and '_', but cannot start with a digit.
 * Inputs: ch - the character; is_first - 1 if it would be the first one.
 * Returns: 1 if it can, 0 otherwise.
 */
static int is_name_char(char ch, int is_first)
{
    /* ctype functions need an unsigned char value, hence the cast. */
    if (isalpha((unsigned char)ch) || ch == '_') {
        return 1;
    }
    return !is_first && isdigit((unsigned char)ch);
}

/*
 * expand_variable - replace $NAME with the value of environment variable
 * NAME (found with getenv). An unknown variable becomes an empty string.
 * A '$' that is not followed by a name, as in "$" or "5$", stays a '$'.
 * Inputs: line - the input; pos - index of the '$' (moved past the name);
 *         word, length - the word being built (the value is added to it).
 * Returns: 0 on success, -1 if the word becomes too long.
 */
static int expand_variable(const char *line, int *pos, char *word, int *length)
{
    char name[MAX_LINE];
    int name_length = 0;
    int i = *pos + 1;                /* first character after the '$' */
    const char *value;
    int k;

    while (is_name_char(line[i], name_length == 0)) {
        name[name_length] = line[i];
        name_length++;
        i++;
    }
    name[name_length] = '\0';
    *pos = i;

    if (name_length == 0) {
        return append_char(word, length, '$');
    }
    value = getenv(name);
    if (value == NULL) {
        return 0;
    }
    for (k = 0; value[k] != '\0'; k++) {
        if (append_char(word, length, value[k]) == -1) {
            return -1;
        }
    }
    return 0;
}

/*
 * read_word - read one word starting at line[*pos], removing its quotes
 * and expanding $NAME variables.
 *   - Inside '...' or "..." spaces and operators are ordinary characters,
 *     so  echo "a | b"  gives the single argument  a | b.
 *   - $NAME is expanded outside quotes and inside "...", but NOT inside
 *     '...' (single quotes keep everything exactly as typed, like bash).
 * Inputs: line - the input; pos - index to start at (moved past the word);
 *         tok - the token to fill.
 * Returns: WORD_OK, WORD_VANISHED (an unquoted variable that was empty, such
 *          as $NOT_SET - bash drops such a word completely) or WORD_ERROR.
 */
static int read_word(const char *line, int *pos, struct token *tok)
{
    int i = *pos;
    int length = 0;
    char quote = 0;        /* 0 when outside quotes, otherwise ' or " */
    int was_quoted = 0;    /* 1 if the word contained any quotes, e.g. "" */
    int result = 0;

    while (line[i] != '\0' && result == 0) {
        char ch = line[i];

        if (quote == 0 && (is_space(ch) || is_operator_char(ch))) {
            break;                      /* end of the word */
        }
        if (quote == 0 && (ch == '"' || ch == '\'')) {
            quote = ch;                 /* opening quote: not part of the word */
            was_quoted = 1;
            i++;
        } else if (quote != 0 && ch == quote) {
            quote = 0;                  /* closing quote: not part of the word */
            i++;
        } else if (ch == '$' && quote != '\'') {
            result = expand_variable(line, &i, tok->text, &length);
        } else {
            result = append_char(tok->text, &length, ch);
            i++;
        }
    }

    if (result == -1) {
        return WORD_ERROR;
    }
    if (quote != 0) {
        fprintf(stderr, "eduishell: syntax error: missing closing %c\n", quote);
        return WORD_ERROR;
    }
    tok->text[length] = '\0';
    tok->type = TOKEN_WORD;
    *pos = i;
    if (length == 0 && !was_quoted) {
        return WORD_VANISHED;
    }
    return WORD_OK;
}

/*
 * tokenize - cut a whole line into tokens.
 * Inputs: line - the input; tokens - the list to fill.
 * Returns: 0 on success, -1 on error (a message has been printed).
 */
static int tokenize(const char *line, struct token_list *tokens)
{
    int pos = 0;
    int result;
    struct token *tok;

    tokens->count = 0;
    while (1) {
        while (is_space(line[pos])) {
            pos++;
        }
        if (line[pos] == '\0') {
            return 0;
        }
        if (tokens->count == MAX_TOKENS) {
            fprintf(stderr, "eduishell: too many words on one line (max %d)\n", MAX_TOKENS);
            return -1;
        }

        tok = &tokens->items[tokens->count];
        if (is_operator_char(line[pos])) {
            read_operator(line, &pos, tok);
            tokens->count++;
            continue;
        }

        result = read_word(line, &pos, tok);
        if (result == WORD_ERROR) {
            return -1;
        }
        /* A vanished word (empty $VARIABLE) is simply not added. */
        if (result == WORD_OK) {
            tokens->count++;
        }
    }
}

/* ==================== Step 2: grouping tokens ==================== */

/*
 * syntax_error - print a message naming the token where parsing failed.
 * Inputs: tokens - the token list; index - position of the bad token
 *         (index == tokens->count means "the end of the line").
 * Returns: nothing.
 */
static void syntax_error(struct token_list *tokens, int index)
{
    if (index < tokens->count) {
        fprintf(stderr, "eduishell: syntax error near '%s'\n", tokens->items[index].text);
    } else {
        fprintf(stderr, "eduishell: syntax error near end of line\n");
    }
}

/*
 * clear_command - reset a command to "no words, no redirections".
 * Inputs: cmd - the command.
 * Returns: nothing.
 */
static void clear_command(struct command *cmd)
{
    cmd->argc = 0;
    cmd->argv[0] = NULL;
    cmd->input_file = NULL;
    cmd->output_file = NULL;
    cmd->append_output = 0;
}

/*
 * add_argument - append one word to a command's argv.
 * Inputs: cmd - the command; word - the word (points into the token list).
 * Returns: 0 on success, -1 if there are too many words.
 */
static int add_argument(struct command *cmd, char *word)
{
    /* Keep one slot free for the NULL that execvp() needs at the end. */
    if (cmd->argc == MAX_ARGS - 1) {
        fprintf(stderr, "eduishell: too many arguments (max %d)\n", MAX_ARGS - 1);
        return -1;
    }
    cmd->argv[cmd->argc] = word;
    cmd->argc++;
    cmd->argv[cmd->argc] = NULL;
    return 0;
}

/*
 * set_redirection - remember a  < file,  > file  or  >> file  for a command.
 * If the same kind is given twice, the last one wins.
 * Inputs: cmd - the command; type - which operator; file_name - the file.
 * Returns: nothing.
 */
static void set_redirection(struct command *cmd, enum token_type type, char *file_name)
{
    if (type == TOKEN_INPUT) {
        cmd->input_file = file_name;
    } else {
        cmd->output_file = file_name;
        cmd->append_output = (type == TOKEN_APPEND);
    }
}

/*
 * parse_command - build one command from tokens[start .. end-1].
 * This range contains only words and redirections (no | ; &).
 * Inputs: tokens - the token list; start, end - the range; cmd - to fill.
 * Returns: 0 on success, -1 on a syntax error.
 */
static int parse_command(struct token_list *tokens, int start, int end, struct command *cmd)
{
    int i = start;
    struct token *tok;

    clear_command(cmd);
    while (i < end) {
        tok = &tokens->items[i];
        if (tok->type == TOKEN_WORD) {
            if (add_argument(cmd, tok->text) == -1) {
                return -1;
            }
            i = i + 1;
        } else {
            /* A redirection operator must be followed by a file name. */
            if (i + 1 >= end || tokens->items[i + 1].type != TOKEN_WORD) {
                syntax_error(tokens, i + 1);
                return -1;
            }
            set_redirection(cmd, tok->type, tokens->items[i + 1].text);
            i = i + 2;
        }
    }

    /* Catches "| ls", "ls |", "ls | | wc" and "; ls" ... */
    if (start == end) {
        syntax_error(tokens, end);
        return -1;
    }
    /* ... and "> file" with no command in front of it. */
    if (cmd->argc == 0) {
        fprintf(stderr, "eduishell: syntax error: redirection without a command\n");
        return -1;
    }
    return 0;
}

/*
 * parse_pipeline - build one pipeline from tokens[start .. end-1],
 * cutting it into commands at every |.
 * Inputs: tokens - the token list; start, end - the range; pl - to fill.
 * Returns: 0 on success, -1 on error.
 */
static int parse_pipeline(struct token_list *tokens, int start, int end, struct pipeline *pl)
{
    int i;
    int cmd_start = start;

    pl->num_cmds = 0;
    pl->background = 0;
    for (i = start; i <= end; i++) {
        /* A command ends at a | or at the end of the range. */
        if (i < end && tokens->items[i].type != TOKEN_PIPE) {
            continue;
        }
        if (pl->num_cmds == MAX_CMDS) {
            fprintf(stderr, "eduishell: too many commands in a pipeline (max %d)\n", MAX_CMDS);
            return -1;
        }
        if (parse_command(tokens, cmd_start, i, &pl->cmds[pl->num_cmds]) == -1) {
            return -1;
        }
        pl->num_cmds++;
        cmd_start = i + 1;
    }
    return 0;
}

/*
 * is_separator - check whether a token ends a pipeline (; or &).
 * Inputs: tok - the token.
 * Returns: 1 if it is ; or &, 0 otherwise.
 */
static int is_separator(struct token *tok)
{
    return tok->type == TOKEN_SEMICOLON || tok->type == TOKEN_BACKGROUND;
}

/*
 * parse_tokens - group the whole token list into pipelines, cutting it at
 * every ; and &.
 * Inputs: tokens - the token list; cmdline - the result to fill.
 * Returns: 0 on success, -1 on error.
 */
static int parse_tokens(struct token_list *tokens, struct command_line *cmdline)
{
    int i;
    int start = 0;
    struct pipeline *pl;

    cmdline->num_pipelines = 0;
    for (i = 0; i <= tokens->count; i++) {
        /* A pipeline ends at a ; or & or at the end of the line. */
        if (i < tokens->count && !is_separator(&tokens->items[i])) {
            continue;
        }
        if (start == i) {
            /* Nothing before this ; or &. At the very end that is fine ("ls ;"). */
            if (i < tokens->count) {
                syntax_error(tokens, i);
                return -1;
            }
            break;
        }
        if (cmdline->num_pipelines == MAX_PIPELINES) {
            fprintf(stderr, "eduishell: too many commands on one line (max %d)\n", MAX_PIPELINES);
            return -1;
        }
        pl = &cmdline->pipelines[cmdline->num_pipelines];
        if (parse_pipeline(tokens, start, i, pl) == -1) {
            return -1;
        }
        pl->background = (i < tokens->count && tokens->items[i].type == TOKEN_BACKGROUND);
        cmdline->num_pipelines++;
        start = i + 1;
    }
    return 0;
}

/*
 * parse_line - turn one input line into a command_line.
 * Inputs: line - the text typed by the user;
 *         tokens - storage for the tokens (the commands point into it,
 *                  so it must live as long as cmdline is used);
 *         cmdline - the result.
 * Returns: 0 on success (cmdline->num_pipelines may be 0 for an empty line),
 *          -1 on error (a message has already been printed).
 */
int parse_line(const char *line, struct token_list *tokens, struct command_line *cmdline)
{
    cmdline->num_pipelines = 0;
    if (tokenize(line, tokens) == -1) {
        return -1;
    }
    return parse_tokens(tokens, cmdline);
}
