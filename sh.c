#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include "./jobs.h"

#define BUFFER_SIZE 1024
#define MAX_TOKENS 512

// global variables for job control
static job_list_t *job_list;        // list of all background and stopped jobs
static pid_t fg_pid = -1;           // pid of current foreground job
static int next_jid = 1;            // next available jid
static int foreground_job_id = -1;  // jid of current foreground job

// command types
enum command_type {
    CMD_REGULAR,  // regular
    CMD_FG,       // fg
    CMD_BG,       // bg
    CMD_JOBS      // jobs commands
};

// struct to hold parsed command information
struct parse_result {
    char *command_path;          // full path to executable
    char *input_file;            // input redirection
    char *output_file;           // output redirection
    int append_mode;             // 1 if >>, 0 if >
    char *argv[MAX_TOKENS];      // args
    int background;              // if command ends with &
    enum command_type cmd_type;  // type of command
    int job_id;                  // jid for fg/bg commands (-1 if N/A)
};

/*
 * gets info about a job: suchas pid and current state
 *
 * jid - jid to look up
 * pid - pointer to store pid
 * state - pointer to store process state
 * returns 0 if success, -1 if error
 */
static int get_job_info(int jid, pid_t *pid, process_state_t *state) {
    if (!job_list || !pid || !state) {
        return -1;
    }

    *pid = get_job_pid(job_list, jid);
    if (*pid == -1) {
        return -1;
    }

    process_state_t current_state = RUNNING;
    if (update_job_pid(job_list, *pid, current_state) == -1) {
        return -1;
    }

    if (update_job_pid(job_list, *pid, STOPPED) != -1) {
        *state = STOPPED;
        update_job_pid(job_list, *pid, current_state);
    } else {
        *state = RUNNING;
    }

    return 0;
}

/*
 * sends a signal to a process group
 *
 * pid - process ID
 * signal - signal num to send
 * returns 0 on success, -1 on error
 */
static int send_signal_to_job(pid_t pid, int signal) {
    if (pid <= 0) {
        return -1;
    }

    if (kill(-pid, signal) < 0) {
        perror("kill");
        return -1;
    }
    return 0;
}

/*
 * gives terminal control to process group
 *
 * pgid - process group id to receive control
 * returns 0 on success, -1 on error
 */
static int give_terminal_to(pid_t pgid) {
    if (pgid <= 0) {
        return -1;
    }

    if (tcsetpgrp(STDIN_FILENO, pgid) < 0) {
        perror("tcsetpgrp");
        return -1;
    }
    return 0;
}

/*
 * returns terminal control to shell
 *
 * returns 0 on success, -1 on error
 */
static int take_terminal_control(void) {
    pid_t shell_pgid = getpgrp();
    if (shell_pgid < 0) {
        perror("getpgrp");
        return -1;
    }

    if (tcsetpgrp(STDIN_FILENO, shell_pgid) < 0) {
        perror("tcsetpgrp");
        return -1;
    }
    return 0;
}

/*
 * waits for a child and handles status
 *
 * pid - pid to wait for
 * status - pointer to store process status
 * returns -1 on error, 0 on regular termination, 1 if stopped
 */
static int wait_for_job(pid_t pid, int *status) {
    if (!status || pid <= 0) {
        return -1;
    }

    if (waitpid(pid, status, WUNTRACED) < 0) {
        perror("waitpid");
        return -1;
    }

    return WIFSTOPPED(*status) ? 1 : 0;
}

/*
 * checks and reaps any background processes that have changed state
 * also updates job list and prints status messages for finished processes
 *
 * returns 1 if any process was reaped, 0 if none
 */
static int reap_background_processes(void) {
    pid_t pid;
    int status;
    int reaped = 0;

    // check for any child that has changed state
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        int jid = get_job_jid(job_list, pid);

        // skip if not a tracked job and not the foreground process
        if (jid == -1 && pid != fg_pid) {
            continue;
        }

        // handle normal termination
        if (WIFEXITED(status)) {
            if (jid > 0) {
                fprintf(stdout, "[%d] (%d) terminated with exit status %d\n",
                        jid, pid, WEXITSTATUS(status));
                remove_job_pid(job_list, pid);
            }
        }
        // handle termination by signal
        else if (WIFSIGNALED(status)) {
            if (jid > 0) {
                fprintf(stdout, "[%d] (%d) terminated by signal %d\n", jid, pid,
                        WTERMSIG(status));
                remove_job_pid(job_list, pid);
            } else {
                fprintf(stdout, "(%d) terminated by signal %d\n", pid,
                        WTERMSIG(status));
            }
        }
        // handle stop by signal
        else if (WIFSTOPPED(status)) {
            if (jid == -1) {
                // add to job list
                char command_buf[BUFFER_SIZE];
                snprintf(command_buf, sizeof(command_buf), "%s",
                         fg_pid == pid ? "fg_command" : "bg_command");

                if (add_job(job_list, next_jid, pid, STOPPED, command_buf) ==
                    0) {
                    jid = next_jid++;
                    fprintf(stdout, "[%d] (%d) suspended by signal %d\n", jid,
                            pid, WSTOPSIG(status));
                } else {
                    fprintf(stderr, "Error: Failed to add job to job list\n");
                }
            } else {
                // update job status
                update_job_pid(job_list, pid, STOPPED);
                fprintf(stdout, "[%d] (%d) suspended by signal %d\n", jid, pid,
                        WSTOPSIG(status));
            }
        }
        // handle SIGCONT
        else if (WIFCONTINUED(status)) {
            if (jid > 0) {
                update_job_pid(job_list, pid, RUNNING);
                fprintf(stdout, "[%d] (%d) resumed\n", jid, pid);
            }
        }
        reaped = 1;
    }

    // check for waitpid errors
    if (pid < 0 && errno != ECHILD) {
        perror("waitpid");
    }

    return reaped;
}

/*
 * initializes signal handlers for shell process
 * sets up SIGINT, SIGTSTP, and SIGTTOU to ignore, and SIGQUIT to default
 */
static void init_signal_handlers(void) {
    // signals to ignore
    const int signals[] = {SIGINT, SIGTSTP, SIGTTOU};
    const int num_signals = sizeof(signals) / sizeof(signals[0]);

    // set up signal handlers
    for (int i = 0; i < num_signals; i++) {
        if (signal(signals[i], SIG_IGN) == SIG_ERR) {
            fprintf(stderr, "Error: Failed to set handler for signal %d\n",
                    signals[i]);
            exit(1);
        }
    }

    if (signal(SIGQUIT, SIG_DFL) == SIG_ERR) {
        fprintf(stderr, "Error: Failed to set SIGQUIT handler\n");
        exit(1);
    }
}

/*
 * checks if a string contains only whitespace characters/is empty
 *
 * str - string to be checked
 * returns 1 if string is empty or all whitespace, 0 otherwise
 */
static int is_empty_or_whitespace(const char *str) {
    if (!str) {
        return 1;
    }

    while (*str) {
        if (!isspace((unsigned char)*str)) {
            return 0;
        }
        str++;
    }
    return 1;
}

/*
 * parses input into command components and stores in result struct
 * handles command parsing, I/O redirection, and background process
 *
 * buffer - input string to be parsed
 * result - pointer to structure to store parsed command information
 * returns 0 on success, -1 on parsing error
 */
static int parse(char buffer[BUFFER_SIZE], struct parse_result *result) {
    if (!buffer || !result || strlen(buffer) >= BUFFER_SIZE) {
        return -1;
    }

    char *tokens[MAX_TOKENS];
    int token_count = 0;
    char *token;
    int has_input_redirect = 0;
    int has_output_redirect = 0;

    // initialize result structure
    memset(result, 0, sizeof(struct parse_result));
    result->job_id = -1;

    token = strtok(buffer, " \t\n");
    if (!token) {
        return -1;
    }

    // handle job control commands
    if (strcmp(token, "fg") == 0 || strcmp(token, "bg") == 0) {
        result->cmd_type = (strcmp(token, "fg") == 0) ? CMD_FG : CMD_BG;
        token = strtok(NULL, " \t\n");

        if (!token || token[0] != '%') {
            fprintf(stderr, "ERROR: Expected %%<job-id>\n");
            return -1;
        }

        char *job_str = token + 1;
        if (!isdigit((unsigned char)*job_str)) {
            fprintf(stderr, "ERROR: Invalid job ID\n");
            return -1;
        }

        result->job_id = atoi(job_str);
        result->command_path = (result->cmd_type == CMD_FG) ? "fg" : "bg";
        result->argv[0] = result->command_path;
        result->argv[1] = NULL;

        // check for extra args
        if (strtok(NULL, " \t\n")) {
            fprintf(stderr, "ERROR: Too many arguments\n");
            return -1;
        }

        return 0;
    }

    // tokenize remaining
    while (token && token_count < MAX_TOKENS - 1) {
        tokens[token_count++] = token;
        token = strtok(NULL, " \t\n");
    }
    tokens[token_count] = NULL;

    // check for background
    if (token_count > 0 && strcmp(tokens[token_count - 1], "&") == 0) {
        result->background = 1;
        token_count--;
        tokens[token_count] = NULL;
    }

    // process command and args
    int arg_count = 0;
    for (int i = 0; i < token_count; i++) {
        // handle input redirections
        if (strcmp(tokens[i], "<") == 0) {
            if (has_input_redirect || i + 1 >= token_count) {
                fprintf(stderr, "ERROR: Invalid input redirection\n");
                return -1;
            }
            result->input_file = tokens[++i];
            has_input_redirect = 1;
            continue;
        }

        // handle output redirections
        if (strcmp(tokens[i], ">") == 0 || strcmp(tokens[i], ">>") == 0) {
            if (has_output_redirect || i + 1 >= token_count) {
                fprintf(stderr, "ERROR: Invalid output redirection\n");
                return -1;
            }
            result->output_file = tokens[++i];
            result->append_mode = (tokens[i - 1][1] == '>');
            has_output_redirect = 1;
            continue;
        }

        // handle command and args
        if (!result->command_path) {
            result->command_path = tokens[i];
            // extract command name from path for argv[0]
            char *last_slash = strrchr(tokens[i], '/');
            result->argv[arg_count++] = last_slash ? last_slash + 1 : tokens[i];
        } else {
            result->argv[arg_count++] = tokens[i];
        }
    }

    // validate command
    if (!result->command_path) {
        fprintf(stderr, "ERROR: No command specified\n");
        return -1;
    }

    result->argv[arg_count] = NULL;
    return 0;
}

/*
 * sets up input/output redirections
 * handles both input (<) and output (>, >>) redirection
 *
 * result - pointer to parsed command information struct
 * returns 0 on success, -1 on error
 */
static int setup_redirections(struct parse_result *result) {
    if (!result) {
        return -1;
    }

    // handle input redirection
    if (result->input_file) {
        if (close(STDIN_FILENO) < 0) {
            perror("close");
            return -1;
        }
        if (open(result->input_file, O_RDONLY) < 0) {
            perror("open");
            return -1;
        }
    }

    // handle output redirection
    if (result->output_file) {
        if (close(STDOUT_FILENO) < 0) {
            perror("close");
            return -1;
        }

        int flags = O_WRONLY | O_CREAT;
        flags |= result->append_mode ? O_APPEND : O_TRUNC;

        if (open(result->output_file, flags, 0644) < 0) {
            perror("open");
            return -1;
        }
    }

    return 0;
}

/*
 * handles execution of shell built-in commands
 *
 * result - pointer to parsed command info
 * returns 1 if command was handled, 0 if not a builtin, -1 on error
 */
static int handle_builtin(struct parse_result *result) {
    if (!result || !result->command_path || !result->argv[0]) {
        return -1;
    }

    // handle fg/bg commands
    if (result->cmd_type == CMD_FG || result->cmd_type == CMD_BG) {
        pid_t pid;
        process_state_t state;

        // validate job existence
        if (get_job_info(result->job_id, &pid, &state) < 0) {
            fprintf(stderr, "ERROR: No such job\n");
            return -1;
        }

        if (result->cmd_type == CMD_FG) {
            // move to fg
            if (give_terminal_to(pid) < 0 ||
                send_signal_to_job(pid, SIGCONT) < 0) {
                take_terminal_control();
                return -1;
            }

            fg_pid = pid;
            foreground_job_id = result->job_id;
            update_job_pid(job_list, pid, RUNNING);

            int status;
            int wait_result = wait_for_job(pid, &status);

            if (wait_result < 0) {
                take_terminal_control();
                return -1;
            }

            // handle status change for job
            if (wait_result == 1 && WIFSTOPPED(status)) {
                fprintf(stdout, "[%d] (%d) suspended by signal %d\n",
                        result->job_id, pid, WSTOPSIG(status));
                update_job_pid(job_list, pid, STOPPED);
            } else if (WIFSIGNALED(status)) {
                fprintf(stdout, "(%d) terminated by signal %d\n", pid,
                        WTERMSIG(status));
                remove_job_jid(job_list, result->job_id);
            } else {
                remove_job_jid(job_list, result->job_id);
            }

            fg_pid = -1;
            foreground_job_id = -1;

            return take_terminal_control() == 0 ? 1 : -1;
        } else {  // bg
            if (state != STOPPED) {
                fprintf(stderr, "ERROR: Job is already running\n");
                return -1;
            }

            if (send_signal_to_job(pid, SIGCONT) < 0) {
                return -1;
            }

            update_job_pid(job_list, pid, RUNNING);
            return 1;
        }
    }

    // skips built-in handling for paths containing '/'
    if (strchr(result->command_path, '/')) {
        return 0;
    }

    // handle other built-in commands
    if (strcmp(result->argv[0], "exit") == 0) {
        if (result->argv[1]) {
            fprintf(stderr, "ERROR: exit command takes no arguments\n");
            return -1;
        }
        cleanup_job_list(job_list);
        exit(0);
    }

    if (strcmp(result->argv[0], "jobs") == 0) {
        jobs(job_list);
        return 1;
    }

    if (strcmp(result->argv[0], "cd") == 0) {
        if (!result->argv[1]) {
            fprintf(stderr, "ERROR: cd requires a directory argument\n");
            return -1;
        }
        if (result->argv[2]) {
            fprintf(stderr, "ERROR: cd takes only one argument\n");
            return -1;
        }
        if (chdir(result->argv[1]) < 0) {
            perror("cd");
            return -1;
        }
        return 1;
    }

    if (strcmp(result->argv[0], "ln") == 0) {
        if (!result->argv[1] || !result->argv[2]) {
            fprintf(stderr,
                    "ERROR: ln requires source and destination arguments\n");
            return -1;
        }
        if (result->argv[3]) {
            fprintf(stderr, "ERROR: ln takes exactly two arguments\n");
            return -1;
        }
        if (link(result->argv[1], result->argv[2]) < 0) {
            perror("ln");
            return -1;
        }
        return 1;
    }

    if (strcmp(result->argv[0], "rm") == 0) {
        if (!result->argv[1]) {
            fprintf(stderr, "ERROR: rm requires a file argument\n");
            return -1;
        }
        if (result->argv[2]) {
            fprintf(stderr, "ERROR: rm takes only one argument\n");
            return -1;
        }
        if (unlink(result->argv[1]) < 0) {
            perror("rm");
            return -1;
        }
        return 1;
    }

    return 0;
}

/*
 * primary shell loop that processes commands and manages jobs
 * initializes shell environment, reads and executes commands,
 * and handles process/job control
 *
 * returns 0 on normal exit, 1 on error
 */
int main(void) {
    char buffer[BUFFER_SIZE];
    struct parse_result result;
    ssize_t bytes_read;

    // initialize shell environment
    init_signal_handlers();

    job_list = init_job_list();
    if (!job_list) {
        fprintf(stderr, "Error: Failed to initialize job list\n");
        return 1;
    }

    // main loop
    while (1) {
        // reap background processes before prompt
        reap_background_processes();

        // print prompt if compiled with PROMPT defined
#ifdef PROMPT
        if (printf("33sh> ") < 0 || fflush(stdout) < 0) {
            fprintf(stderr, "Error: Failed to display prompt\n");
            cleanup_job_list(job_list);
            return 1;
        }
#endif

        bytes_read = read(STDIN_FILENO, buffer, BUFFER_SIZE - 1);
        if (bytes_read < 0) {
            perror("read");
            cleanup_job_list(job_list);
            return 1;
        }

        // handle EOF
        if (bytes_read == 0) {
            cleanup_job_list(job_list);
            return 0;
        }

        // null terminate and handle newline
        buffer[bytes_read] = '\0';
        if (bytes_read > 0 && buffer[bytes_read - 1] == '\n') {
            buffer[bytes_read - 1] = '\0';
        }

        // skip empty lines
        if (is_empty_or_whitespace(buffer)) {
            continue;
        }

        // parse command
        if (parse(buffer, &result) < 0) {
            continue;
        }

        // handle built-ins
        int builtin_status = handle_builtin(&result);
        if (builtin_status != 0) {
            continue;
        }

        // fork child
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            continue;
        }

        if (pid == 0) {  // child
            if (setpgid(0, 0) < 0) {
                perror("setpgid");
                exit(1);
            }

            // reset sig handlers to default
            const int signals[] = {SIGINT, SIGTSTP, SIGTTOU};
            for (int i = 0; i < 3; i++) {
                if (signal(signals[i], SIG_DFL) == SIG_ERR) {
                    perror("signal");
                    exit(1);
                }
            }

            // set up terminal control for fg
            if (!result.background) {
                if (tcsetpgrp(STDIN_FILENO, getpid()) < 0) {
                    perror("tcsetpgrp");
                    exit(1);
                }
            }

            // set up I/O redirections
            if (setup_redirections(&result) < 0) {
                exit(1);
            }

            // execute command
            execv(result.command_path, result.argv);
            perror("execv");
            exit(1);
        }

        // parent
        if (setpgid(pid, pid) < 0 && errno != EACCES) {
            perror("setpgid");
        }

        if (!result.background) {
            // handle fg process
            fg_pid = pid;
            if (tcsetpgrp(STDIN_FILENO, pid) < 0) {
                perror("tcsetpgrp");
            }

            int status;
            if (waitpid(pid, &status, WUNTRACED) < 0) {
                perror("waitpid");
            } else {
                // handle status change
                if (WIFSTOPPED(status)) {
                    if (add_job(job_list, next_jid, pid, STOPPED,
                                result.command_path) == 0) {
                        fprintf(stdout, "[%d] (%d) suspended by signal %d\n",
                                next_jid, pid, WSTOPSIG(status));
                        next_jid++;
                    } else {
                        fprintf(stderr,
                                "Error: Failed to add job to job list\n");
                    }
                } else if (WIFSIGNALED(status)) {
                    fprintf(stdout, "(%d) terminated by signal %d\n", pid,
                            WTERMSIG(status));
                }
            }

            // return terminal to shell
            fg_pid = -1;
            if (tcsetpgrp(STDIN_FILENO, getpgrp()) < 0) {
                perror("tcsetpgrp");
            }
        } else {
            // handle bg process
            if (add_job(job_list, next_jid, pid, RUNNING,
                        result.command_path) == 0) {
                fprintf(stdout, "[%d] (%d)\n", next_jid, pid);
                next_jid++;
            } else {
                fprintf(stderr, "Error: Failed to add job to job list\n");
            }
        }
    }

    cleanup_job_list(job_list);
    return 0;
}