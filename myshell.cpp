#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>   // open(), creat()
#include <signal.h>
#include <errno.h>
#include <termios.h>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>

// Helper structure to hold command information
struct Command {
    std::vector<char*> args;
    std::string inputFile;
    std::string outputFile;
    bool hasRedirect = false;
    bool background = false;     // run in background if true (applies to whole pipeline)
    std::string raw;             // raw command text for job listing
};

// Job control
enum JobStatus { RUNNING, STOPPED, DONE };
struct Job {
    int id;
    pid_t pgid;
    std::string cmdline;
    JobStatus status;
};

static std::vector<Job> jobs;
static int next_job_id = 1;
static pid_t shell_pgid;
static struct termios shell_tmodes;

// --- Forward declarations ---
int handle_builtin(Command& cmd);
int launch_command(Command& cmd);
void execute_pipeline(std::vector<Command>& commands);
std::vector<Command> parse_line_advanced(std::string line);
void cleanup_commands(std::vector<Command>& commands);
void init_shell();
void sigchld_handler(int sig);
void add_job(pid_t pgid, const std::string &cmdline, JobStatus status);
int job_index_by_id(int id);
void print_jobs();
void bring_job_foreground(int id, bool cont);
void continue_job_background(int id, bool cont);

// --- Initialize shell for job control ---
void init_shell() {
    // Do nothing if not running interactively
    if (!isatty(STDIN_FILENO)) return;

    // Ignore interactive and job-control signals (shell retains control)
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    // Install SIGCHLD handler via sigaction
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, nullptr) < 0) {
        perror("sigaction");
    }

    // Put shell in its own process group
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EACCES) {
        perror("Couldn't put the shell in its own process group");
    }

    // Grab control of the terminal
    if (tcsetpgrp(STDIN_FILENO, shell_pgid) < 0) {
        // not fatal: report but continue
        // perror("tcsetpgrp");
    }

    if (tcgetattr(STDIN_FILENO, &shell_tmodes) < 0) {
        // perror("tcgetattr");
    }
}

// --- Signal handler for SIGCHLD: reap children and update job list ---
void sigchld_handler(int sig) {
    (void)sig; // avoid unused-parameter warning
    int saved_errno = errno;
    pid_t pid;
    int status;
    // Reap in a loop
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        // Find job containing pid (by pgid)
        pid_t pgid = getpgid(pid);
        if (pgid <= 0) continue;
        for (auto &job : jobs) {
            if (job.pgid == pgid) {
                if (WIFSTOPPED(status)) {
                    job.status = STOPPED;
                } else if (WIFCONTINUED(status)) {
                    job.status = RUNNING;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    job.status = DONE;
                }
                break;
            }
        }
    }
    errno = saved_errno;
}

void add_job(pid_t pgid, const std::string &cmdline, JobStatus status) {
    Job j;
    j.id = next_job_id++;
    j.pgid = pgid;
    j.cmdline = cmdline;
    j.status = status;
    jobs.push_back(j);
}

int job_index_by_id(int id) {
    for (size_t i = 0; i < jobs.size(); ++i) {
        if (jobs[i].id == id) return (int)i;
    }
    return -1;
}

void print_jobs() {
    for (auto &job : jobs) {
        const char* st = (job.status == RUNNING) ? "Running" : (job.status == STOPPED) ? "Stopped" : "Done";
        printf("[%d] %s\t\t%s\n", job.id, st, job.cmdline.c_str());
    }
}

void bring_job_foreground(int id, bool cont) {
    int idx = job_index_by_id(id);
    if (idx < 0) {
        fprintf(stderr, "myshell: fg: %%%d: no such job\n", id);
        return;
    }
    Job job = jobs[idx];
    // Move job to foreground
    tcsetpgrp(STDIN_FILENO, job.pgid);
    if (cont) {
        if (kill(-job.pgid, SIGCONT) < 0) perror("kill (SIGCONT)");
    }
    // Wait for job
    int status;
    waitpid(-job.pgid, &status, WUNTRACED);
    // Restore shell to foreground
    tcsetpgrp(STDIN_FILENO, shell_pgid);

    // Update job status
    if (WIFSTOPPED(status)) {
        jobs[idx].status = STOPPED;
    } else {
        jobs[idx].status = DONE;
    }
}

void continue_job_background(int id, bool cont) {
    int idx = job_index_by_id(id);
    if (idx < 0) {
        fprintf(stderr, "myshell: bg: %%%d: no such job\n", id);
        return;
    }
    Job &job = jobs[idx];
    if (cont) {
        if (kill(-job.pgid, SIGCONT) < 0) perror("kill (SIGCONT)");
    }
    job.status = RUNNING;
}

// --- Built-in Command Handler ---
int handle_builtin(Command& cmd) {
    if (cmd.args.empty() || cmd.args[0] == NULL) return 1;

    if (strcmp(cmd.args[0], "exit") == 0) {
        return 0; // Signal to exit
    } else if (strcmp(cmd.args[0], "cd") == 0) {
        if (cmd.args.size() < 2 || cmd.args[1] == NULL) {
            fprintf(stderr, "myshell: expected argument to \"cd\"\n");
        } else {
            if (chdir(cmd.args[1]) != 0) {
                perror("myshell");
            }
        }
        return 1; // Continue shell loop
    } else if (strcmp(cmd.args[0], "jobs") == 0) {
        print_jobs();
        return 1;
    } else if (strcmp(cmd.args[0], "fg") == 0) {
        if (cmd.args.size() < 2 || cmd.args[1] == NULL) {
            fprintf(stderr, "myshell: fg: usage: fg %%jobid\n");
        } else {
            // accept %N or N
            std::string a(cmd.args[1]);
            if (a.size() > 0 && a[0] == '%') a = a.substr(1);
            int id = atoi(a.c_str());
            bring_job_foreground(id, true);
        }
        return 1;
    } else if (strcmp(cmd.args[0], "bg") == 0) {
        if (cmd.args.size() < 2 || cmd.args[1] == NULL) {
            fprintf(stderr, "myshell: bg: usage: bg %%jobid\n");
        } else {
            std::string a(cmd.args[1]);
            if (a.size() > 0 && a[0] == '%') a = a.substr(1);
            int id = atoi(a.c_str());
            continue_job_background(id, true);
        }
        return 1;
    }
    return -1; // Not a built-in
}

// --- Execute a single command with potential redirection ---
int launch_command(Command& cmd) {
    pid_t pid;
    int status;

    pid = fork();
    if (pid == 0) {
        // Child process: put into its own process group
        setpgid(0, 0);
        // Restore default signals in child
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);

        // Handle I/O redirection
        if (!cmd.inputFile.empty()) {
            int fd_in = open(cmd.inputFile.c_str(), O_RDONLY);
            if (fd_in < 0) { perror("open input"); exit(EXIT_FAILURE); }
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }
        if (!cmd.outputFile.empty()) {
            int fd_out = open(cmd.outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_out < 0) { perror("open output"); exit(EXIT_FAILURE); }
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }

        // Execute the command
        if (cmd.args.size() == 0 || cmd.args[0] == NULL) {
            exit(EXIT_FAILURE);
        }
        execvp(cmd.args[0], cmd.args.data());
        if (errno == ENOENT) {
            fprintf(stderr, "%s: command not found\n", cmd.args[0]);
        } else {
            fprintf(stderr, "%s: %s\n", cmd.args[0], strerror(errno));
        }
        _exit(EXIT_FAILURE);
    } else if (pid < 0) {
        perror("myshell");
    } else {
        // Parent: set child's process group
        setpgid(pid, pid);
        if (cmd.background) {
            // background: add to jobs and don't wait
            add_job(pid, cmd.raw.empty() ? (cmd.args[0] ? cmd.args[0] : "") : cmd.raw, RUNNING);
            printf("[%d] %d\n", next_job_id - 1, pid);
        } else {
            // Foreground: give terminal to child and wait for it
            tcsetpgrp(STDIN_FILENO, pid);
            do {
                waitpid(pid, &status, WUNTRACED);
            } while (!WIFEXITED(status) && !WIFSIGNALED(status) && !WIFSTOPPED(status));
            // Restore control to shell
            tcsetpgrp(STDIN_FILENO, shell_pgid);
            if (WIFSTOPPED(status)) {
                add_job(pid, cmd.raw.empty() ? (cmd.args[0] ? cmd.args[0] : "") : cmd.raw, STOPPED);
            }
        }
    }
    return 1;
}

// --- Execute a pipeline of commands (e.g., cmd1 | cmd2) ---
void execute_pipeline(std::vector<Command>& commands) {
    int num_cmds = commands.size();
    if (num_cmds == 0) return;

    std::vector<int> pipefds; // dynamic storage for fds
    if (num_cmds > 1) pipefds.resize(2 * (num_cmds - 1), -1);
    pid_t pgid = 0;
    std::string combined_raw;
    for (auto &c : commands) {
        if (!combined_raw.empty()) combined_raw += " | ";
        combined_raw += c.raw;
    }
    bool background = commands.back().background;

    // Create all necessary pipes
    for (int i = 0; i < num_cmds - 1; ++i) {
        if (pipe(&pipefds[i * 2]) < 0) {
            perror("pipe error");
            return;
        }
    }

    for (int i = 0; i < num_cmds; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            // Put child into process group
            if (i == 0) {
                setpgid(0, 0);
            } else {
                setpgid(0, pgid);
            }
            // Restore default signals
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);

            // Handle input redirection if it's the first command and specified
            if (i == 0 && !commands[i].inputFile.empty()) {
                int fd_in = open(commands[i].inputFile.c_str(), O_RDONLY);
                if (fd_in < 0) { perror("open input"); exit(EXIT_FAILURE); }
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            } else if (i != 0) {
                // Redirect stdin to the read end of the previous pipe
                dup2(pipefds[(i-1)*2], STDIN_FILENO);
            }

            // Handle output redirection if it's the last command and specified
            if (i == num_cmds - 1 && !commands[i].outputFile.empty()) {
                int fd_out = open(commands[i].outputFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd_out < 0) { perror("open output"); exit(EXIT_FAILURE); }
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            } else if (i != num_cmds - 1) {
                // Redirect stdout to the write end of the current pipe
                dup2(pipefds[i*2 + 1], STDOUT_FILENO);
            }

            // Close all pipe ends in the child process
            for (size_t j = 0; j < pipefds.size(); ++j) {
                if (pipefds[j] != -1) close(pipefds[j]);
            }

            // Exec the command (ensure non-empty)
            if (commands[i].args.empty() || commands[i].args[0] == NULL) {
                exit(EXIT_FAILURE);
            }
            execvp(commands[i].args[0], commands[i].args.data());
            if (errno == ENOENT) {
                fprintf(stderr, "%s: command not found\n", commands[i].args[0]);
            } else {
                fprintf(stderr, "%s: %s\n", commands[i].args[0], strerror(errno));
            }
            _exit(EXIT_FAILURE);

        } else if (pid < 0) {
            perror("fork error");
            // Close pipes
            for (size_t j = 0; j < pipefds.size(); ++j) if (pipefds[j] != -1) close(pipefds[j]);
            return;
        } else {
            // Parent
            if (i == 0) {
                pgid = pid;
            }
            // set child's pgid (ignore error if already set in child)
            setpgid(pid, pgid);
        }
    }

    // Parent process: Close all pipe ends
    for (size_t i = 0; i < pipefds.size(); ++i) if (pipefds[i] != -1) close(pipefds[i]);

    if (background) {
        add_job(pgid, combined_raw, RUNNING);
        printf("[%d] %d\n", next_job_id - 1, pgid);
    } else {
        // Foreground: give terminal to process group and wait
        if (isatty(STDIN_FILENO)) tcsetpgrp(STDIN_FILENO, pgid);
        int status;
        // Wait for the process group (wait for any member)
        waitpid(-pgid, &status, WUNTRACED);
        // Restore control to shell
        if (isatty(STDIN_FILENO)) tcsetpgrp(STDIN_FILENO, shell_pgid);
        if (WIFSTOPPED(status)) {
            add_job(pgid, combined_raw, STOPPED);
        }
    }
}

// --- Advanced Command Line Parser (Detects '>', '<', '|', '&') ---
std::vector<Command> parse_line_advanced(std::string line) {
    std::vector<Command> commands;
    // Tokenize respecting single and double quotes (preserve quoted text as one token,
    // and strip the surrounding quotes)
    auto tokenize = [](const std::string &s) {
        std::vector<std::string> toks;
        size_t i = 0, n = s.size();
        while (i < n) {
            // skip whitespace
            while (i < n && isspace((unsigned char)s[i])) ++i;
            if (i >= n) break;
            if (s[i] == '\'' || s[i] == '"') {
                char q = s[i++];
                std::string cur;
                while (i < n && s[i] != q) {
                    cur.push_back(s[i++]);
                }
                if (i < n && s[i] == q) ++i; // consume closing quote
                toks.push_back(cur);
            } else {
                std::string cur;
                while (i < n && !isspace((unsigned char)s[i])) {
                    cur.push_back(s[i++]);
                }
                toks.push_back(cur);
            }
        }
        return toks;
    };
    Command currentCmd;
    currentCmd.raw = "";
    // Split respecting quoted tokens
    std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) return commands;

    // Detect trailing '&' for background
    bool trailing_background = false;
    if (!tokens.empty() && tokens.back() == "&") {
        trailing_background = true;
        tokens.pop_back();
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "|") {
            // finalize current command
            if (!currentCmd.raw.empty()) currentCmd.raw += " | ";
            // ensure argv is NULL-terminated before pushing (fixes "Bad address")
            if (!currentCmd.args.empty() && currentCmd.args.back() != NULL) {
                currentCmd.args.push_back(NULL);
            }
            commands.push_back(currentCmd);
            currentCmd = Command();
            currentCmd.raw = "";
        } else if (tokens[i] == ">") {
            if (i + 1 < tokens.size()) {
                currentCmd.outputFile = tokens[++i];
                currentCmd.hasRedirect = true;
            } else {
                fprintf(stderr, "myshell: syntax error near unexpected token `\\n'\n");
                return {};
            }
        } else if (tokens[i] == "<") {
            if (i + 1 < tokens.size()) {
                currentCmd.inputFile = tokens[++i];
                currentCmd.hasRedirect = true;
            } else {
                fprintf(stderr, "myshell: syntax error near unexpected token `\\n'\n");
                return {};
            }
        } else {
            // Add argument, allocating memory
            // strip surrounding single/double quotes if present
            std::string tok = tokens[i];
            if (tok.size() >= 2) {
                if ((tok.front() == '\'' && tok.back() == '\'') ||
                    (tok.front() == '"' && tok.back() == '"')) {
                    tok = tok.substr(1, tok.size() - 2);
                }
            }
            if (!currentCmd.raw.empty()) currentCmd.raw += " ";
            currentCmd.raw += tok;
            char* c_token = new char[tok.length() + 1];
            strcpy(c_token, tok.c_str());
            currentCmd.args.push_back(c_token);
         }
    }

    // Add the last command if it exists
    if (!currentCmd.args.empty()) {
        currentCmd.args.push_back(NULL);
        commands.push_back(currentCmd);
    }

    if (!commands.empty() && trailing_background) {
        commands.back().background = true;
    }

    return commands;
}

void cleanup_commands(std::vector<Command>& commands) {
    for (auto& cmd : commands) {
        for (char* arg : cmd.args) {
            if (arg != NULL) {
                delete[] arg;
            }
        }
        cmd.args.clear();
    }
    // Remove DONE jobs from list
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const Job &j){ return j.status == DONE; }), jobs.end());
}

// --- Main shell loop ---
void shell_loop() {
    std::string line;

    init_shell();

    while (true) {
        std::cout << "my_shell> ";
        std::getline(std::cin, line);
        if (std::cin.eof()) break;
        if (line.empty()) continue;

        std::vector<Command> commands = parse_line_advanced(line);

        if (!commands.empty()) {
            // If single command and builtin, handle
            if (commands.size() == 1 && handle_builtin(commands[0]) != -1) {
                // Handled internally
            } else if (commands.size() > 1) {
                // pipeline
                execute_pipeline(commands);
            } else {
                // Single external command with potential redirection
                launch_command(commands[0]);
            }
        }

        cleanup_commands(commands);
    }
}

int main() {
    shell_loop();
    return EXIT_SUCCESS;
}