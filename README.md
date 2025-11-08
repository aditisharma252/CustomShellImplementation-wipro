# Custom Shell — Capstone Project (Assignment 2)

Lightweight POSIX-compatible custom shell implementing basic command execution, piping, redirection, background/foreground process management and job control.

**##Prerequisites**

Make sure your system has:

Linux / WSL (Windows Subsystem for Linux)

g++ compiler (C++17 or above)

Git (for version control)

**Day-wise Implementation Log**
Day 1 – Project Setup & Basic Shell Loop
Created project folder structure (wipro_shell_project/custom_shell/src/)
Initialized Git repository and connected with GitHub
Implemented the basic shell loop:
Display prompt (myshell>)
Read input line from user using getline()
Handle exit command (exit)
Verified simple loop works without crashing

Day 2 – Command Parsing
Implemented input parsing using a tokenizer that handles:
Whitespace separation
Quoted strings (“abc def”)
Created a Command structure to hold:
args, infile, outfile, append
Added logic to split pipelines (|) and detect background execution (&)

Day 3 – Process Management (Foreground & Background)
Added support for background process execution using &
Implemented process tracking with a Job struct:
Stores pgid, command line, and running status
Added background job list using map<pid_t, Job>
Shell prints process ID for background jobs and returns to prompt immediately

Day 4 – Piping and Redirection
Implemented pipes using pipe() and connected multiple processes:
e.g., ls -l | grep cpp | wc -l
Added input/output redirection using dup2() system calls:
Input < file.txt
Output > file.txt or append >> file.txt
Ensured proper closing of unused pipe ends to avoid deadlocks

Day 5 – Job Control (Foreground/Background Switching)
Implemented job control commands:
jobs → list all background jobs
fg <pgid> → bring a background job to foreground
bg <pgid> → resume a stopped job in background
Added signal handling (SIGCHLD) using sigaction() to detect job completion or stop events
Automatically updates job list and prints status (e.g., “Done” or “Stopped”)

## Features
- Execute external programs via `execvp`.
- Builtins: `cd`, `exit`, `jobs`, `fg`, `bg`.
- Input (`<`) and output (`>`) redirection.
- Pipelines (e.g. `cmd1 | cmd2 | cmd3`).
- Background execution with `&` (trailing): e.g. `sleep 60 &`.
- Job control:
  - `jobs` — list tracked jobs (Running / Stopped / Done).
  - `fg %N` — bring job N to foreground.
  - `bg %N` — continue stopped job N in background.
- Proper signal handling for job control (SIGCHLD reaping, terminal process-group management).
- Simple quoted-token-aware parser (single/double quotes preserved and stripped).

## Limitations / Notes
- Parser is simple: no support for advanced shell features (globbing, variable expansion, `&&`, `||`, `;`, command substitution).
- Append redirection (`>>`) not implemented.
- Job ids are the bracketed numbers shown by `jobs` (use `%N` or `N` with `fg`/`bg`).
- Interactive tests requiring `Ctrl-C`/`Ctrl-Z` must be performed in an actual terminal session (not redirected input).

## Build & Run

1. Open the project directory containing `myshell.cpp` and the Makefile:
   ```
   cd /home/aditi_117/wipro_shell_project/custom_shell/src/src
   ```
   OR if you have Makefile
   Build:
   ```
   make
   ```

3. Run:
   ```
   ./myshell
   ```
   or
   ```
   make run
   ```

4. Check for memory issues:
   ```
   make valgrind
   ```

5. Clean build artifacts:
   ```
   make clean
   ```

## Quick Test Commands

Run these at the `my_shell>` prompt:

- Basic:
  ```
  pwd
  echo hello world
  ls -la
  ```

- Redirection:
  ```
  printf "one\ntwo\nthree\n" > f.txt
  cat < f.txt
  ```

- Pipeline:
  ```
  printf "1\n2\n3\n4\n5\n" | awk '{print $1*2}' | sed 's/^/N:/'
  ```

- Background + jobs:
  ```
  sleep 60 &
  jobs
  ```

- Suspend/Resume (interactive):
  ```
  sleep 120    # press Ctrl-Z to stop
  jobs
  bg %<id>     # resume in background
  fg %<id>     # bring to foreground
  ```

- Error case:
  ```
  no_such_cmd   # prints "no_such_cmd: command not found"
  ```

## Debugging / Development Tips
- If you see unexpected crashes, run under Valgrind to inspect allocation issues:
  ```
  valgrind --leak-check=full --track-origins=yes ./myshell
  ```
- Ensure you use the job id shown by `jobs` (the `[N]` number) with `fg`/`bg`.

## Code Layout
- `src/src/myshell.cpp` — main implementation.

## License
Project source provided for educational use.
