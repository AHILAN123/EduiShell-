#!/bin/bash
#
# run_tests.sh - compare EduiShell with bash.
#
# Each test feeds the same input (through stdin) to bash and to our shell
# and compares what they print on STDOUT. stderr is not compared, because
# error messages are worded differently ("bash: line 1: foo: command not
# found" vs "eduishell: foo: command not found").
#
# Both shells run inside the same, freshly emptied directory, so files that
# a test creates (out.txt, users.txt, ...) never leak into the next test.
#
# Usage:  make test     (or: bash tests/run_tests.sh)

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
SHELL_UNDER_TEST="$TESTS_DIR/../eduishell"
TIME_LIMIT=5          # seconds; a test that runs longer counts as a hang

if [ ! -x "$SHELL_UNDER_TEST" ]; then
    echo "eduishell is not built yet - run 'make' first"
    exit 1
fi

work_root=$(mktemp -d)
trap 'rm -rf "$work_root"' EXIT
case_dir="$work_root/case"

passed=0
failed=0

# run_case PROGRAM INPUT NAME
#   Runs INPUT through PROGRAM in an empty $case_dir.
#   Leaves stdout in $work_root/<name>.out, stderr in $work_root/<name>.err
#   (name is "bash" or "ours") and the exit code in $last_exit.
#   The input arrives through a pipe, unless $input_from_file is "yes":
#   then it comes from a regular file, like  ./eduishell < script.txt
run_case() {
    local program="$1"
    local input="$2"
    local name="$3"

    rm -rf "$case_dir"
    mkdir "$case_dir"
    printf '%s\n' "$input" > "$work_root/input.txt"
    if [ "$input_from_file" = "yes" ]; then
        (cd "$case_dir" && timeout "$TIME_LIMIT" "$program" < "$work_root/input.txt" \
            > "$work_root/$name.out" 2> "$work_root/$name.err")
    else
        (cd "$case_dir" && cat "$work_root/input.txt" | timeout "$TIME_LIMIT" "$program" \
            > "$work_root/$name.out" 2> "$work_root/$name.err")
    fi
    last_exit=$?
}

input_from_file="no"

# crashed - true if our shell timed out (124) or was killed by a signal (>128).
crashed() {
    [ "$last_exit" -eq 124 ] || [ "$last_exit" -gt 128 ]
}

# finish_test NAME EXPECTED_FILE
#   Prints PASS or FAIL for one test and counts it. The test failed if
#   $problem is not empty; when the output was wrong, a diff is shown
#   ("<" lines are what was expected, ">" lines are what we printed).
finish_test() {
    if [ -z "$problem" ]; then
        printf 'PASS  %s\n' "$1"
        passed=$((passed + 1))
        return
    fi
    printf 'FAIL  %s  (%s)\n' "$1" "$problem"
    failed=$((failed + 1))
    if [ "$problem" = "wrong output" ]; then
        diff "$2" "$work_root/ours.out" | sed 's/^/        /'
    fi
}

# compare_with_bash INPUT
#   Runs INPUT through bash and through our shell. Sets $problem to an
#   empty string if both printed the same thing on stdout.
compare_with_bash() {
    run_case bash "$1" bash
    run_case "$SHELL_UNDER_TEST" "$1" ours
    problem=""
    if crashed; then
        problem="hung or crashed, exit code $last_exit"
    elif ! cmp -s "$work_root/bash.out" "$work_root/ours.out"; then
        problem="wrong output"
    fi
}

# check_same NAME INPUT
#   PASS if our shell prints exactly what bash prints.
check_same() {
    compare_with_bash "$2"
    finish_test "$1" "$work_root/bash.out"
}

# check_error NAME INPUT
#   Like check_same, but our shell must ALSO print an error message on stderr.
check_error() {
    compare_with_bash "$2"
    if [ -z "$problem" ] && [ ! -s "$work_root/ours.err" ]; then
        problem="no error message on stderr"
    fi
    finish_test "$1" "$work_root/bash.out"
}

# check_output NAME INPUT EXPECTED
#   For things bash does differently on purpose (for example 'history' is
#   off in a non-interactive bash): compare with a fixed expected text.
check_output() {
    run_case "$SHELL_UNDER_TEST" "$2" ours
    printf '%s\n' "$3" > "$work_root/expected.out"
    problem=""
    if crashed; then
        problem="hung or crashed, exit code $last_exit"
    elif ! cmp -s "$work_root/expected.out" "$work_root/ours.out"; then
        problem="wrong output"
    fi
    finish_test "$1" "$work_root/expected.out"
}

# check_exit_code NAME INPUT CODE
#   PASS if our shell exits with exit status CODE after reading INPUT.
check_exit_code() {
    run_case "$SHELL_UNDER_TEST" "$2" ours
    problem=""
    if [ "$last_exit" -ne "$3" ]; then
        problem="exit code was $last_exit, expected $3"
    fi
    finish_test "$1" /dev/null
}

# check_ctrl_c NAME INPUT WORD...
#   Starts our shell on INPUT in its own process group (as a terminal
#   would). INPUT should start a "sleep 3"; after 0.5 s we send SIGINT
#   (what Ctrl-C does) to the whole group. PASS if the shell survived,
#   finished well before the 3 seconds were over (so the sleep was
#   stopped) and printed every WORD.
#   "set -m" (job control) matters: without it bash starts & jobs with
#   SIGINT ignored, and the test could not tell a working shell from one
#   that does nothing about Ctrl-C.
check_ctrl_c() {
    local name="$1"
    local start_ms end_ms shell_pid word
    rm -rf "$case_dir"
    mkdir "$case_dir"
    printf '%s\n' "$2" > "$work_root/input.txt"
    shift 2

    start_ms=$(date +%s%3N)
    set -m
    "$SHELL_UNDER_TEST" < "$work_root/input.txt" > "$work_root/ours.out" 2>&1 &
    shell_pid=$!
    set +m
    sleep 0.5
    kill -INT -- "-$shell_pid"
    wait "$shell_pid"
    last_exit=$?
    end_ms=$(date +%s%3N)

    problem=""
    if [ "$last_exit" -ne 0 ]; then
        problem="the shell itself died (exit code $last_exit)"
    elif [ $((end_ms - start_ms)) -ge 2500 ]; then
        problem="sleep was not interrupted"
    fi
    for word in "$@"; do
        if [ -z "$problem" ] && ! grep -q "$word" "$work_root/ours.out"; then
            problem="'$word' was not printed"
        fi
    done
    finish_test "$name" /dev/null
}

echo "=== Phase 1: core loop and external commands ==="
check_same  "simple command"            "uname -s"
check_same  "command with arguments"    "ls -a /"
check_same  "several lines"             "$(printf 'uname -s\nls /\n/bin/echo third line')"
check_same  "extra spaces and tabs"     "$(printf '  /bin/echo \t lots    of \t spaces  ')"
check_error "nonexistentcmd"            "nonexistentcmd"
check_same  "keeps going after error"   "$(printf 'nonexistentcmd\n/bin/echo still running')"
check_same  "empty input"               ""
check_same  "only spaces"               "      "

echo "=== Phase 2: parsing ==="
check_same  "double quotes"             'echo "hello   world"'
check_same  "single quotes"             "echo 'single   quoted'"
check_same  "quotes glued together"     "echo \"a\"'b'c\"d e\""
check_same  "one quote inside another"  "echo \"it's\" 'say \"hi\"'"
check_same  "empty quoted arguments"    "echo \"\" x ''"
check_same  "quoted operators are text" "echo \"a|b\" '>' \"x;y&z\" '>>'"
check_error "| alone"                   "|"
check_error "pipe at the end"           "ls |"
check_error "pipe at the start"         "| ls"
check_error "two pipes in a row"        "echo a | | wc"
check_error "> alone"                   ">"
check_error "> with no file"            "echo a >"
check_error ">> with no file"           "echo a >>"
check_error "< with no file"            "cat <"
check_error "redirection with no command" "> f"
check_error "> followed by |"           "ls > | wc"
check_error "; alone"                   ";"
check_error "empty command between ;"   "echo a ; ; echo b"
check_error "operators without spaces"  "echo a;;"
check_error "& alone"                   "&"
check_error "missing closing quote"     'echo "unclosed'
check_output "still running after a syntax error" "$(printf '|\necho after')" "after"

echo "=== Phase 3: built-in commands ==="
check_same  "echo builtin"              "echo hello    world"
check_same  "echo -n"                   "$(printf 'echo -n no newline\necho')"
check_same  "echo with no arguments"    "echo"
check_same  "pwd"                       "pwd"
check_same  "cd then pwd"               "$(printf 'cd /tmp\npwd')"
check_same  "cd alone goes to HOME"     "$(printf 'cd\npwd')"
check_same  "cd .."                     "$(printf 'cd /usr/bin\ncd ..\npwd')"
check_same  "cd affects later commands" "$(printf 'cd /\nls')"
check_same  "cd updates \$PWD"          "$(printf 'cd /tmp\nprintenv PWD')"
check_error "cd to a missing directory" "$(printf 'cd /no/such/dir\npwd')"
check_error "cd with too many args"     "$(printf 'cd /tmp /usr\npwd')"
check_same  "exit stops the shell"      "$(printf 'echo before\nexit\necho after')"
check_exit_code "exit 3 sets exit code" "exit 3" 3
check_exit_code "Ctrl-D exits with 0"   "echo bye" 0
check_output "history" "$(printf 'echo a\n\n   \necho b\nhistory')" \
    "$(printf 'a\nb\n    1  echo a\n    2  echo b\n    3  history')"
check_output "history keeps only the last 50" \
    "$(for i in $(seq 1 55); do echo "echo $i"; done; echo history)" \
    "$(seq 1 55; for i in $(seq 7 55); do printf '%5d  echo %d\n' "$i" "$i"; done; printf '%5d  history' 56)"

echo "=== Phase 4: redirection ==="
check_same  "> creates a file"          "$(printf 'echo hi > out.txt\ncat out.txt')"
check_same  "> overwrites"              "$(printf 'echo first > out.txt\necho second > out.txt\ncat out.txt')"
check_same  ">> appends"                "$(printf 'echo hi >> out.txt\necho there >> out.txt\ncat out.txt')"
check_same  "< reads a file"            "$(printf 'echo from file > in.txt\ncat < in.txt')"
check_same  "< and > together"          "$(printf 'echo b > in.txt\necho a >> in.txt\nsort < in.txt > out.txt\ncat out.txt')"
check_same  "redirection without spaces" "$(printf 'echo hi>out.txt\necho more>>out.txt\ncat<out.txt')"
check_same  "redirection before command" "$(printf '> out.txt echo hi\ncat out.txt')"
check_same  "external program to file"  "$(printf 'ls / > list.txt\ncat list.txt')"
check_same  "wc -l < /etc/passwd"       "wc -l < /etc/passwd"
check_same  "new files get mode 644"    "$(printf 'echo x > f.txt\nstat -c %%a f.txt')"
check_same  "> on a builtin (pwd)"      "$(printf 'pwd > p.txt\ncat p.txt')"
check_error "< missing file"            "$(printf 'cat < missing.txt\necho next')"
check_error "> into a missing folder"   "$(printf 'echo hi > /no/such/dir/x\necho next')"
check_error "> into a read-only place"  "$(printf 'echo hi > /proc/version\necho next')"

echo "=== Phase 5: pipelines ==="
check_same  "two commands"              "echo hello world | wc -w"
check_same  "ls -l /tmp | grep -v total | wc -l" "ls -l /tmp | grep -v total | wc -l"
check_same  "ls | grep c | wc -l"       "$(printf 'cd /bin\nls | grep c | wc -l')"
check_same  "pipes without spaces"      "echo a b c|wc -w|cat"
check_same  "5-stage pipeline"          "cat /etc/passwd | cut -d: -f1 | sort | head -5 | wc -l"
check_same  "6-stage pipeline"          "cat /etc/passwd | cut -d: -f7 | sort | uniq -c | sort -rn | head -3"
check_same  "16 commands (MAX_CMDS)"    "echo deep | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat"
check_output "17 commands is refused"   "$(printf 'echo deep | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat | cat\necho done')" "done"
check_same  "pipeline + redirection"    "$(printf 'echo b > in.txt\necho a >> in.txt\necho b >> in.txt\nsort < in.txt | uniq > out.txt\ncat out.txt')"
check_same  "users.txt example"         "$(printf 'cat < /etc/passwd | cut -d: -f1 | sort | head -5 > users.txt\ncat users.txt')"
check_same  "unused pipe ends closed (yes | head)" "yes | head -3"
check_same  "reader finishes first"     "seq 1 100000 | head -1"
check_same  "builtin in a pipeline"     "pwd | wc -c"
check_same  "cd in a pipeline is a no-op" "$(printf 'cd / | cat\npwd')"
check_error "missing command in pipeline" "echo a | nonexistentcmd | wc -l"
check_output "help lists every builtin" "help | grep -c -E '^  (cd|pwd|echo|history|help|exit) '" "6"
check_output "history in a pipeline"    "$(printf 'echo a\nhistory | tail -1')" "$(printf 'a\n    2  history | tail -1')"

echo "=== Phase 6: ; & \$VARIABLES and Ctrl-C ==="
check_same  'echo "hello   world" $HOME' 'echo "hello   world" $HOME'
check_same  "users.txt example with ;"  "cat < /etc/passwd | cut -d: -f1 | sort | head -5 > users.txt; cat users.txt"
check_same  "cd /tmp; pwd"              "cd /tmp; pwd"
check_same  ">> example with ;"         "echo hi >> out.txt; echo there >> out.txt; cat out.txt"
check_same  "; without spaces"          "echo a;echo b;echo c"
check_same  "trailing ;"                "echo a;"
check_same  "exit in the middle"        "echo a; exit; echo b"
check_error "syntax error stops line"   "echo a; ls |"
check_same  "\$USER and quoting rules"  "echo \$USER '\$HOME' \"\$HOME/docs\" \"[\$NO_SUCH_VAR]\" \$NO_SUCH_VAR end"
check_same  "variable glued to text"    'echo pre$USER-post'
check_same  "lone dollar signs"         "echo \$ \"a\$\" '\$' 5\$"
check_same  "unset variable vanishes"   'echo $NO_SUCH_VAR | wc -c'
check_same  "\"\" is kept, \$EMPTY is not" 'printf "[%s]" "" $NO_SUCH_VAR x'
check_same  "variable as command name"  'cd $HOME; pwd'
check_same  "variable in a file name"   'echo hi > out_$USER.txt; cat out_$USER.txt'
check_same  "& does not wait"           "sleep 0.3 & echo started"
check_same  "background job runs"       "$(printf 'echo from background > bg.txt &\nsleep 0.3\ncat bg.txt')"
check_same  "background pipeline"       "$(printf 'seq 1 5 | wc -l > bg.txt &\nsleep 0.3\ncat bg.txt')"
check_same  "no zombies left after &"   "$(printf 'sleep 0.1 &\nsleep 0.4\nps -eo stat= | grep -c ^Z')"
check_error "& prints the PID"          "sleep 0.1 &"
check_same  "cd with & does not move"   "$(printf 'cd / &\nsleep 0.1\npwd')"
check_ctrl_c "Ctrl-C stops the command, not the shell" \
    "$(printf 'sleep 3\necho survived')" survived
check_ctrl_c "Ctrl-C does not stop background jobs" \
    "$(printf "sh -c 'sleep 1; echo bg-alive' &\nsleep 3\nsleep 1\necho survived")" bg-alive survived

echo "=== Input from a file instead of a pipe ==="
input_from_file="yes"
check_same  "script file with builtins" "$(printf 'echo one\npwd | cat\necho two; echo three\nhelp > /dev/null\necho four')"
check_same  "script file with pipelines" "$(printf 'ls / | wc -l\ncd /tmp; pwd\nyes | head -2\necho end')"
check_same  "command reads rest of script" "$(printf 'head -n 1\necho second\necho third')"
input_from_file="no"

echo
echo "Total: $passed passed, $failed failed"
[ "$failed" -eq 0 ]
