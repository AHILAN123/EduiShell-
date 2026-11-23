# Makefile for EduiShell
#
#   make           build ./eduishell
#   make test      run tests/run_tests.sh (compares our output with bash)
#   make valgrind  run a batch of commands under valgrind to look for leaks
#   make clean     delete everything the build produced

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -g

TARGET = eduishell
SRCS = main.c parser.c execute.c builtins.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Every object file depends on its .c file and on the shared header.
%.o: %.c shell.h
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	bash tests/run_tests.sh

# The shell's own output goes to /dev/null so only valgrind's report is shown.
# You get one report for the shell and one for each child that ran a builtin
# (children that exec another program leave valgrind at that point).
valgrind: $(TARGET)
	valgrind --leak-check=full --error-exitcode=1 ./$(TARGET) < tests/valgrind_input.txt > /dev/null

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all test valgrind clean
