Main Structure of my Self-Implemented Terminal:
- The main function does the primary function of running the REPL, reading from the user, and then calling the appropriate function/s
- Parse function processes the input buffer:
    - Identifies and stores I/O redirections in the result struct
    - Stores the full file path to the command
    - Builds the argv array for command execution
    - Handles background process requests and job control commands
- Built-in command handler processes built-ins:
    - Manages jobs, fg, bg commands (new in shell 2)
    - Handles cd, ln, rm, and exit commands (same as shell 1)
    - Returns status indicating if command was built-in
- For non-built-in commands:
    - Forks child process
    - Child sets up process group and signal handlers
    - Child handles I/O redirection
    - Child executes commands
    - Parent manages job control and waits as needed
- Returns to beginning of loop to read next command

Bugs:
- No known bugs

Extra Features:
- No features outside of handout instructions

How to compile:
- Run make clean all
