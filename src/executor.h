#pragma once
#include <iostream>
#include <filesystem>
#include <system_error>
#include <fstream>
#include "job.h"
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#include <cstring>
#include <signal.h>
#include <seccomp.h>
#include <chrono>
#include <sys/resource.h>

struct RunResult {
    std::string output;
    std::string error;
    int exit_code;
};

struct ChildArgs {
    std::string binary_path;
    int outpipe[2];
    int errpipe[2];
};

int child_fn(void* arg) {
    ChildArgs* args = static_cast<ChildArgs*>(arg);

    close(args->outpipe[0]);
    close(args->errpipe[0]);

    //stdout to outpipe's write end
    dup2(args->outpipe[1], STDOUT_FILENO);
    //stderr to errpipe's write end
    dup2(args->errpipe[1], STDERR_FILENO);

    close(args->outpipe[1]);
    close(args->errpipe[1]);

    //Add a filter context - SCMP_ACT_ALLOW means allow everything by default
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);

    //Add rules for syscalls to be blocked
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(fork), 0);

    //Load the filter into the kernel
    seccomp_load(ctx);

    //Free the context
    seccomp_release(ctx);

    struct rlimit mem_limit;
    mem_limit.rlim_cur = 256 * 1024 * 1024; //256mb soft limit
    mem_limit.rlim_max = 256 * 1024 * 1024; //256mb hard limit
    setrlimit(RLIMIT_AS, &mem_limit);

    char* exec_args[] = {(char*)args->binary_path.c_str(), NULL};

    //replaces the current process with a new one
    int status = execvp(args->binary_path.c_str(), exec_args);
    std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
    _exit(1);
}


class Executor {
private:
    int create_cgroup(int job_id) {
        std::ofstream subtree("/sys/fs/cgroup/cgroup.subtree_control");
        if (!subtree) {
            std::cerr << "Failed to open subtree_control" << std::endl;
        } else {
            subtree << "+memory";
            subtree.close();
        }

        namespace fs = std::filesystem;
        fs::path cgroup_path = "/sys/fs/cgroup/nexec_" + std::to_string(job_id);

        try {
            if (fs::create_directory(cgroup_path)) {
                std::cout << "Cgroup created successfully: " << cgroup_path << std::endl;
            } else {
                std::cout << "Cgroup already exists" << cgroup_path << std::endl;
            }
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Error creating cgroup: " << e.what() << std::endl;
            return 1;
        }
        fs::path memory_path = cgroup_path / "memory.max";
        std::ofstream ofs(memory_path);
        if (!ofs) {
            std::perror("Failed to open memory file");
        }
        ofs << 256000000;
        ofs.close();

        return 0;
    }

    int add_to_cgroup(int job_id, pid_t pid) {
        namespace fs = std::filesystem;
        fs::path cgroup_path = "/sys/fs/cgroup/nexec_" + std::to_string(job_id);
        fs::path procs_path = cgroup_path / "cgroup.procs";

        std::ofstream ofs(procs_path);
        if (!ofs.is_open()) {
            std::cerr << "Failed to open procs" << std::endl;
            return 1;
        }
        ofs << std::to_string(pid);
        ofs.close();
        return 0;
    }

    bool check_oom(int job_id) {
        namespace fs = std::filesystem;
        fs::path cgroup_path = "/sys/fs/cgroup/nexec_" + std::to_string(job_id);
        fs::path memory_events = cgroup_path / "memory.events";
        std::ifstream ifs(memory_events);

        if (!ifs.is_open()) {
            std::cerr << "Failed to open memory.events file" << std::endl;
            return false;
        }

        std::string key;
        int value;
        while (ifs >> key >> value) {
            if (key == "oom_kill" && value > 0) return true;
        }
        return false;
    }

    int cleanup_cgroup(int job_id) {
        std::string path = "/sys/fs/cgroup/nexec_" + std::to_string(job_id);

        if (rmdir(path.c_str()) == 0) {
            std::cout << "cgroup removed: " << path << std::endl;
        } else {
            std::cerr << "Failed to remove cgroup: " << path << std::endl;
            return 1;
        }
        return 0;
    }

public:
    std::string write_source(const Job& job) {
        std::string filename = "/tmp/nexec_" + std::to_string(job.id) + ".cpp";
        std::ofstream code_file(filename);
        code_file << job.source_code << std::endl;
        code_file.close();
        return filename;
    }

    std::string compile(const std::string& source_path, int job_id) {
        std::string binary_path = "/tmp/nexec_" + std::to_string(job_id);

        char* args[] = {
            (char*)"g++",
            (char*)"-o",
            (char*)binary_path.c_str(),
            (char*)source_path.c_str(),
            NULL
        };
        const char* command = "g++";

        pid_t c_pid = fork();

        if (c_pid == -1) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }
         //child process
         if (c_pid == 0) {
            //replaces the current process with a new one
            int status_code = execvp(command, args);

            std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
            _exit(1);
        }
        else {
            int status;
            //wait for child process result
            pid_t result = waitpid(c_pid, &status, 0);

            if (result == -1) {
                throw std::system_error(errno, std::generic_category(), "waitpid failed");
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                throw std::runtime_error("Compilation failed");
            }
        }
        return binary_path;
    }

    RunResult run(const std::string& binary_path, int job_id) {
        const int STACK_SIZE = 1024 * 1024;
        char* stack = new char[STACK_SIZE];

        int outpipe[2];
        int errpipe[2];
        if (pipe(outpipe) == -1 || pipe(errpipe) == -1) {
            perror("pipe() failed");
        }

        ChildArgs args;
        args.binary_path = binary_path;
        args.outpipe[0] = outpipe[0];
        args.outpipe[1] = outpipe[1];
        args.errpipe[0] = errpipe[0];
        args.errpipe[1] = errpipe[1];

        create_cgroup(job_id);

        pid_t pid = clone(child_fn, stack + STACK_SIZE,
                          CLONE_NEWPID | CLONE_NEWNS | SIGCHLD, &args);

        if (pid == -1) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }
        //parent process
        else {
            add_to_cgroup(job_id, pid);

            close(outpipe[1]);
            close(errpipe[1]);

            int status;
            auto start = std::chrono::steady_clock::now();
            while (true) {
                //suspend parent until child changes state
                pid_t result = waitpid(pid, &status, WNOHANG);
                if (result != 0) break; //child exited
                auto elapsed = std::chrono::steady_clock::now() - start;
                if (elapsed >= std::chrono::seconds(5)) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    delete[] stack;
                    return RunResult{"", "Timeout: execution exceeded 5 seconds", -1};
                }
                usleep(10000);
            }

            if (check_oom(job_id)) {
                cleanup_cgroup(job_id);
                delete[] stack;
                return RunResult{"", "Memory Limit Exceeded", -1};
            }

            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                if (sig == SIGSEGV) {
                    cleanup_cgroup(job_id);
                    delete[] stack;
                    return RunResult{"", "Memory limit exceeded", -1};
                }
                cleanup_cgroup(job_id);
                delete[] stack;
                return RunResult{"", "Process killed by signal: " + std::to_string(sig), -1};
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                throw std::runtime_error("Compilation failed");
            }

            char out_buffer[128];
            char err_buffer[128];
            ssize_t outCount = read(outpipe[0], out_buffer, sizeof(out_buffer));
            ssize_t errCount = read(errpipe[0], err_buffer, sizeof(err_buffer));

            if (outCount == -1 || errCount == -1) {
                perror("read() failed");
            }

            std::string out_str(out_buffer, outCount);
            std::string err_str(err_buffer, errCount);

            return RunResult{out_str, err_str, WEXITSTATUS(status)};
        }
    }
};