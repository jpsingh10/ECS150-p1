#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CMDLINE_MAX 512
#define PT_MAX 512
#define ARGS_MAX 16

typedef enum cmd_type {
    BUILTIN_EXIT,
    BUILTIN_CD,
    BUILTIN_PWD,
    NOT_BUILTIN
} CmdType;

typedef enum error_type {
    PARSE_ERR_ARG_OVERFLOW,
    PARSE_ERR_MISSING_CMD,
    PARSE_ERR_NO_OUTPUT,
    PARSE_ERR_MISLOCATED_REDIR,
    LAUNCH_ERR_ACCESS_DIR,
    LAUNCH_ERR_ACCESS_FILE,
    LAUNCH_ERR_CMD_NOT_FOUND,
    NO_ERROR
} ErrorType;

typedef enum redirect_type {
    NO_REDIRECT,
    REDIRECT_TRUNCATE,
    REDIRECT_APPEND
} RedirectType;

/* Possible parsing states. Used for parse_errors function. */
typedef enum parse_state {
    SEEN_PIPE,  // start state
    SEEN_ONE_ARR,
    SEEN_TWO_ARR,
    READING_PROCESS_WHITESPACE,
    READING_PROCESS_ARGS,
    READING_FILENAME,
    READING_FILENAME_WHITESPACE
} ParseState;

typedef struct process {
    pid_t pid;
    int exit_val;

    /* Process command (equal to args[0]). */
    char *cmd;

    /* Process arguments. */
    char *args[ARGS_MAX + 1];

    RedirectType redirect_output;
    char *filename;

    /* File descriptors for input/output streams. */
    int in, out;

    /* Dynamically allocated copy of process string. Saved so it can be
     * deallocated later. */
    char *token_copy;

    struct process *next;
} Process;

/* Prints error message based on error type. */
void handle_error(ErrorType e) {
    switch (e) {
        case PARSE_ERR_ARG_OVERFLOW:
            fprintf(stderr, "Error: too many process arguments\n");
            break;
        case PARSE_ERR_MISSING_CMD:
            fprintf(stderr, "Error: missing command\n");
            break;
        case PARSE_ERR_NO_OUTPUT:
            fprintf(stderr, "Error: no output file\n");
            break;
        case PARSE_ERR_MISLOCATED_REDIR:
            fprintf(stderr, "Error: mislocated output redirection\n");
            break;
        case LAUNCH_ERR_ACCESS_DIR:
            fprintf(stderr, "Error: cannot cd into directory\n");
            break;
        case LAUNCH_ERR_ACCESS_FILE:
            fprintf(stderr, "Error: cannot open output file\n");
            break;
        case LAUNCH_ERR_CMD_NOT_FOUND:
            fprintf(stderr, "Error: command not found\n");
            break;
        case NO_ERROR:
            fprintf(stderr, "THIS SHOULDN'T PRINT! NO ERROR\n");
            break;
    }
}

/* Looks for all parse errors in input. Uses a DFA. */
int parse_errors(char *input) {
    ParseState state = SEEN_PIPE;
    int num_args = 0, max_args = 0;
    for (int i = 0; input[i]; i++) {
        char curr = input[i];
        /* Factored out reading whitespace characters since behavior for most
         * states is the same: stay in the same state. */
        if (isspace(curr)) {
            if (state == READING_PROCESS_ARGS || state == SEEN_PIPE) {
                state = READING_PROCESS_WHITESPACE;
            } else if (state == SEEN_ONE_ARR || state == SEEN_TWO_ARR) {
                state = READING_FILENAME_WHITESPACE;
            }
        } else if (state == SEEN_PIPE) {
            if (curr == '|' || curr == '>') return PARSE_ERR_MISSING_CMD;

            state = READING_PROCESS_ARGS;
            num_args++;
        } else if (state == SEEN_ONE_ARR) {
            if (curr == '|') return PARSE_ERR_NO_OUTPUT;

            if (curr == '>')
                state = SEEN_TWO_ARR;
            else
                state = READING_FILENAME;
        } else if (state == SEEN_TWO_ARR) {
            if (curr == '|' || curr == '>')
                return PARSE_ERR_NO_OUTPUT;
            else
                state = READING_FILENAME;
        } else if (state == READING_PROCESS_WHITESPACE ||
                   state == READING_PROCESS_ARGS) {
            // `|` and `>` transitions are shared between both reading process
            // states.
            if (curr == '|' || curr == '>') {
                state = (curr == '|') ? SEEN_PIPE : SEEN_ONE_ARR;
                max_args = (max_args > num_args) ? max_args : num_args;
                num_args = 0;
            }
            // Switch to reading args if not reading { |, >, (whitespace) }
            // character in READING_PROCESS_WHITESPACE state.
            else if (state == READING_PROCESS_WHITESPACE) {
                num_args++;
                state = READING_PROCESS_ARGS;
            }
        } else if (state == READING_FILENAME ||
                   state == READING_FILENAME_WHITESPACE) {
            if (curr == '|' || curr == '>') return PARSE_ERR_MISLOCATED_REDIR;
            state = READING_FILENAME;
        }
    }

    max_args = (max_args > num_args) ? max_args : num_args;

    if (state == SEEN_PIPE) return PARSE_ERR_MISSING_CMD;
    if (state == SEEN_ONE_ARR || state == SEEN_TWO_ARR ||
        state == READING_FILENAME_WHITESPACE)
        return PARSE_ERR_NO_OUTPUT;
    if (max_args > 16) return PARSE_ERR_ARG_OVERFLOW;
    return NO_ERROR;
}

/* Redirects standard output to file specified by filename, with truncate or
 * append option depending on redirect type. */
bool redirect_stdout(char *filename, RedirectType rt) {
    /* 0_CREAT = create file if doesn't exist, 0_TRUNC = truncate file if does
     * exist. 0644 sets permissions for owner and group. */
    int fd;
    if (rt == REDIRECT_TRUNCATE) {
        fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    } else {
        fd = open(filename, O_RDWR | O_CREAT | O_APPEND, 0644);
    }
    if (fd == -1) return false;
    dup2(fd, STDOUT_FILENO);
    close(fd);
    return true;
}

/* Closes all open fds besides stdin, stderr, and stdout. */
void close_pipes(Process *head) {
    Process *cur = head;
    while (cur) {
        if (cur->in != STDIN_FILENO) close(cur->in);

        if (cur->out != STDOUT_FILENO) close(cur->out);

        cur = cur->next;
    }
}

/* Sets up file streams (pipes and output redirection) before executing process.
 * Head is used to check which fds are open. */
bool setup_fd_table(Process *p, Process *head) {
    dup2(p->in, STDIN_FILENO);

    if (p->redirect_output == NO_REDIRECT)
        dup2(p->out, STDOUT_FILENO);
    else {
        if (!redirect_stdout(p->filename, p->redirect_output)) return false;
    }

    close_pipes(head);
    return true;
}

/* Splits command line input into process strings using "|" delimiter. Modifies
 * original string. */
void tokenize_processes(char str[], char *process_tokens[]) {
    char *token = strtok(str, "|");

    int token_count = 0;

    while (token) {
        process_tokens[token_count] = token;
        token_count++;
        token = strtok(NULL, "|");
    }

    process_tokens[token_count] = NULL;
}

/* Splits process string into arguments using " " delimiter. Modifies original
 * string. */
void tokenize_cmd(char cmd[], char *buf[]) {
    /* Get rid of trailing filename and output redirection stuff. */

    int i = 0;
    while (cmd[i] != '\0') {
        if (cmd[i] == '>') {
            cmd[i] = '\0';
            break;
        }
        i++;
    }

    i = 0;
    char whitespace_delimiters[] = " \f\n\r\t\v";
    char *token = strtok(cmd, whitespace_delimiters);
    while (token) {
        buf[i++] = token;
        token = strtok(NULL, whitespace_delimiters);
    }

    buf[i] = NULL;
}

/* Checks if fd is open. */
bool fd_is_open(int fd) { return fcntl(fd, F_GETFD) != -1; }

/* Initializes process linked list. Does not set up any pipes or fds. */
Process *initialize_processes(char *process_tokens[]) {
    int i = 0;
    Process *head = NULL;
    Process *cur = NULL;
    while (process_tokens[i]) {
        char *cur_token = process_tokens[i];

        /* Using this to look for output redirection symbols and filenames. */
        char *cur_token_copy = strdup(cur_token);

        Process *next = (Process *)malloc(sizeof(Process));
        tokenize_cmd(cur_token, next->args);
        next->cmd = next->args[0];
        next->exit_val = 0;
        next->next = NULL;

        /* Saving this to free later. */
        next->token_copy = cur_token_copy;

        if (!head)
            head = next;
        else
            cur->next = next;
        cur = next;

        cur->in = STDIN_FILENO;
        cur->out = STDOUT_FILENO;

        /* Handle output redirection (both types) */
        cur->redirect_output = NO_REDIRECT;

        if (strstr(cur_token_copy, ">")) {
            if (strstr(cur_token_copy, ">>"))
                cur->redirect_output = REDIRECT_APPEND;
            else
                cur->redirect_output = REDIRECT_TRUNCATE;
            process_tokens[i] = strtok(cur_token_copy, ">");
            char whitespace_and_redir[] = "> \f\n\r\t\v";
            cur->filename = strtok(NULL, whitespace_and_redir);
        }

        i++;
    }

    return head;
}

/* Implements the builtin sls command. */
void sls() {
    DIR *dir;
    struct dirent *dp;
    struct stat sb;
    dir = opendir(".");
    if (dir == NULL) {
        handle_error(LAUNCH_ERR_ACCESS_DIR);
        exit(EXIT_FAILURE);
    }
    while ((dp = readdir(dir)) != NULL) {
        if (dp->d_name[0] != '.')  // Exclude hidden files
        {
            char filename[PT_MAX];
            snprintf(filename, PT_MAX, "%s/%s", ".", dp->d_name);
            if (stat(filename, &sb) == 0) {
                printf("%s (%lld bytes)\n", dp->d_name, (long long)sb.st_size);
            }
        }
    }
    closedir(dir);
    exit(EXIT_SUCCESS);
}

void pwd() {
    char cwd[PT_MAX];
    getcwd(cwd, sizeof(cwd));
    printf("%s\n", cwd);
    exit(EXIT_SUCCESS);
}

void print_result(Process *head, char *input_cpy) {
    Process *cur = head;
    fprintf(stderr, "+ completed '%s' ", input_cpy);
    while (cur) {
        fprintf(stderr, "[%d]", cur->exit_val);
        cur = cur->next;
    }
    fprintf(stderr, "\n");
    free(input_cpy);
}

void run_processes(Process *head, char *input_cpy) {
    Process *cur = head;
    while (cur) {
        char *cmd = cur->cmd;
        /* Still in parent process... */
        if (!strcmp(cmd, "exit")) {
            fprintf(stderr, "Bye...\n");
            print_result(head, input_cpy);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(cmd, "cd")) {
            char *dir_name = cur->args[1];

            /* No need to call exit from here because we're still in the parent
             * process. */
            if (!dir_name || chdir(dir_name) == -1) {
                handle_error(LAUNCH_ERR_ACCESS_DIR);
                cur->exit_val = 1;
            }
        }

        /* Forking */
        else if (!(cur->pid = fork())) {
            /* Here we need to call exit since we're in the child process. */
            if (!setup_fd_table(cur, head)) {
                handle_error(LAUNCH_ERR_ACCESS_FILE);
                exit(EXIT_FAILURE);
            }
            if (!strcmp(cmd, "pwd")) {
                pwd();
            } else if (!strcmp(cmd, "sls")) {
                sls();
            }

            execvp(cmd, cur->args);
            handle_error(LAUNCH_ERR_CMD_NOT_FOUND);
            exit(EXIT_FAILURE);
        }
        cur = cur->next;
    }

    close_pipes(head);

    cur = head;
    int process_return;

    while (cur) {
        /* Only run if cmd != exit and cmd != cd, since those don't fork. */
        if (strcmp(cur->cmd, "exit") && strcmp(cur->cmd, "cd")) {
            waitpid(cur->pid, &process_return, 0);
            cur->exit_val = WEXITSTATUS(process_return);
        }
        cur = cur->next;
    }

    print_result(head, input_cpy);
}

void free_processes(Process *head) {
    Process *cur = head;
    while (cur != NULL) {
        Process *next = cur->next;
        free(cur->token_copy);
        free(cur);
        cur = next;
    }
}

void prompt_get_input(char *input) {
    char *nl;
    /* Print prompt */
    printf("sshell@ucd$ ");
    fflush(stdout);

    /* Get command line */
    fgets(input, CMDLINE_MAX, stdin);

    /* Print command line if stdin is not provided by terminal */
    if (!isatty(STDIN_FILENO)) {
        printf("%s", input);
        fflush(stdout);
    }
    /* Remove trailing newline from command line */
    nl = strchr(input, '\n');
    if (nl) *nl = '\0';
}

int main(void) {
    char input[CMDLINE_MAX];
    char *process_tokens[ARGS_MAX];
    while (1) {
        /* Prompt user for input and store result. */
        prompt_get_input(input);

        /* Check for parsing errors. */
        ErrorType e = parse_errors(input);
        if (e != NO_ERROR) {
            handle_error(e);
            continue;
        }
        /* Copy input to print out later, since tokenizing modifies input. */
        char *input_cpy = strdup(input);

        /* Tokenize input and parse into Process linked list. */
        tokenize_processes(input, process_tokens);
        Process *head = initialize_processes(process_tokens);
        /* Create pipes. */
        Process *cur = head;
        while (cur) {
            if (cur->next) {
                int fd[2];
                pipe(fd);
                cur->out = fd[1];
                cur->next->in = fd[0];
            }
            cur = cur->next;
        }
        run_processes(head, input_cpy);
        free_processes(head);
    }

    return EXIT_SUCCESS;
}