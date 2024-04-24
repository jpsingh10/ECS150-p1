# SShell: Simple Shell

This program implements a simple shell in C. It supports basic built-in
commands, output redirection, and command piping.

## Implementation Details

### Parse Error Checking
Upon receiving input, the shell first calls `parse_error()` to check for any
parsing errors. It does this using a state machine. The state machine keeps
track of what kind of input (whitespace, arguments, etc) it is currently
parsing and transitions depending on which character it next reads from the
command line input, returning an error if the machine reads something invalid. 

(The state machine approach was taken because it was very difficult to account
for all the different parsing error types without calling `strtok()` multiple
times. This approach allows the shell to check for all possible parsing errors
in a single function call with one pass through the input.)

If the input successfully passes the state machine, the shell next calls
`tokenize_processes()` to separate the input into each of its separate process
strings. 

### Process Initialization
Once the shell is finished tokenizing the input into process strings, it calls
`initialize_processes()` which uses the strings to build a `Process` linked
list. The `Process` struct mainly keeps track of:
* what arguments the shell should use when calling it, 
* its exit value,
* output redirection information,
* which file descriptors its `stdin` and `stdout` are connected to,
* and a pointer to the next process in the list.

`initialize_processes()` finishes command parsing by calling `tokenize_cmd()`
on the process string, which separates the string into its command line
arguments. 


### Piping
To pipe commands together, the shell iterates over the `Process` linked list
and creates a pipe for each process (except the last). The pipe's read and
write FDs are stored as fields in the appropriate processes. The parent process
does not close any pipes, this job is done by the children (before calling
`execvp()`).


### Process Execution
The shell first checks for builtin commands (`exit`, `cd`, `pwd`, `sls`).
`exit` and `cd` run directly on the shell process, while `pwd` and `sls` run on
children processes. `exit` should not fork because exiting out of a forked
process would still allow the shell to run, and `cd` should not fork because
the directory change would "die" along with the forked process. `pwd` and `sls`
both fork to make piping/output redirection more convenient. (Realized this
technically isn't necessary after reading the project specs...) 

Next, the shell checks for forked commands. Whenever the shell forks,
`setup_fd_table()` iterates over all processes to close all open pipes. This
function also calls `redirect_stdout()` if necessary to set up output
redirection in the correct mode (truncate or append). Then, either a custom
function or `execvp()` is called depending on whether the command is builtin or
not. 

Forked builtin functions call `exit(EXIT_SUCCESS)` once they are done, while
forked non-builtin functions call `exit(EXIT_FAILURE)` since a successful
`execvp()` call should never return.

### Process Completion and Memory Management
While the forked processes are running, the shell closes all open pipes and
waits for all children processes to finish. After all processes finish, the
shell prints a completion message to `stderr` by calling `print_result()`. This
function takes a copy of the input and prints it along with all the processes'
exit values (found by iterating through the linked list). The function then
frees the memory allocated by the input copy. This guarantees that the memory
will be freed because `print_result()` is always called during
`run_processes()`.

Before the shell loops and prints its next prompt, `free_processes()` is called
on the linked list. This function iterates through the entire linked list and
frees every node along with any of its dynamically allocated fields. The
function makes sure to move to the next node before freeing the current one to
avoid any illegal memory accesses.


