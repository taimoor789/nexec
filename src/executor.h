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
    long long duration_ms;
};

struct ChildArgs {
    std::string binary_path;
    std::string language;
    int job_id;
    int outpipe[2];
    int errpipe[2];
    int stdinpipe[2];
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

    close(args->stdinpipe[1]);  // close write end - child doesn't write
    dup2(args->stdinpipe[0], STDIN_FILENO);  // replace stdin with pipe read end
    close(args->stdinpipe[0]);

    if (args->language != "java") {
        //Add a filter context SCMP_ACT_ALLOW means allow everything by default
        scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);

        //Add rules for syscalls to be blocked
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(fork), 0);

        //Load the filter into the kernel
        seccomp_load(ctx);

        //Free the context
        seccomp_release(ctx);
    }

    char* exec_args[5];
    if (args->language == "python") {
        exec_args[0] = (char*)"/usr/bin/python3";
        exec_args[1] = (char*)"-u";
        exec_args[2] = (char*)args->binary_path.c_str();
        exec_args[3] = NULL;
        execvp("/usr/bin/python3", exec_args);
    } else if (args->language == "java") {
        std::string class_name = "nexec_" + std::to_string(args->job_id);
        exec_args[0] = (char*)"/usr/bin/java";
        exec_args[1] = (char*)"-cp";
        exec_args[2] = (char*)"/tmp";
        exec_args[3] = (char*)class_name.c_str();
        exec_args[4] = NULL;
        execvp("/usr/bin/java", exec_args);
    } else {
        struct rlimit mem_limit;
        mem_limit.rlim_cur = 256 * 1024 * 1024; //256mb soft limit
        mem_limit.rlim_max = 256 * 1024 * 1024; //256mb hard limit
        setrlimit(RLIMIT_AS, &mem_limit);

        exec_args[0] = (char*)args->binary_path.c_str();
        exec_args[1] = NULL;
        execvp(args->binary_path.c_str(), exec_args);
    }
    std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
    _exit(1);
}

class Executor {
private:
    int create_cgroup(int job_id) {
        std::ofstream subtree("/sys/fs/cgroup/cgroup.subtree_control");
        if (subtree) {
            subtree << "+memory";
            subtree.close();
        }

        namespace fs = std::filesystem;
        fs::path cgroup_path = "/sys/fs/cgroup/nexec_" + std::to_string(job_id);

        try {
            fs::create_directory(cgroup_path);
        } catch (const fs::filesystem_error& e) {
            return 1;
        }

        fs::path memory_path = cgroup_path / "memory.max";
        std::ofstream ofs(memory_path);
        if (ofs) {
            ofs << 256000000;
            ofs.close();
        }

        return 0;
    }

    int add_to_cgroup(int job_id, pid_t pid) {
        namespace fs = std::filesystem;
        fs::path procs_path = "/sys/fs/cgroup/nexec_" + std::to_string(job_id) + "/cgroup.procs";

        std::ofstream ofs(procs_path);
        if (ofs.is_open()) {
            ofs << std::to_string(pid);
            ofs.close();
        }
        return 0;
    }

    bool check_oom(int job_id) {
        namespace fs = std::filesystem;
        fs::path memory_events = "/sys/fs/cgroup/nexec_" + std::to_string(job_id) + "/memory.events";
        std::ifstream ifs(memory_events);
        if (!ifs.is_open()) return false;

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
        std::string language = job.language;
        std::string filename;
        if (language == "cpp") {
            filename = "/tmp/nexec_" + std::to_string(job.id) + ".cpp";
        }
        else if (language == "java") {
            std::string class_name = "nexec_" + std::to_string(job.id);
            filename = "/tmp/" + class_name + ".java";
            std::string source = job.source_code;
            size_t pos = source.find("NEXEC_CLASS");
            if (pos != std::string::npos) {
                source.replace(pos, 11, class_name);
            }
            std::ofstream code_file(filename);
            code_file << source << std::endl;
            code_file.close();
            return filename;  //early return
        }
        else {
            filename = "/tmp/nexec_" + std::to_string(job.id) + ".py";
        }
        std::ofstream code_file(filename);
        code_file << job.source_code << std::endl;
        code_file.close();
        return filename;
    }

    std::string compile(const std::string& source_path, int job_id, const std::string& language) {
        if (language == "python") {
            return source_path;
        }
        std::string binary_path = "/tmp/nexec_" + std::to_string(job_id);

        if (language == "cpp") {
            char* args[] = {
                (char*)"g++",
                (char*)"-o",
                (char*)binary_path.c_str(),
                (char*)source_path.c_str(),
                NULL
            };
            const char* command = "g++";

            int compile_err[2];
            pipe(compile_err);

            pid_t c_pid = fork();

            if (c_pid == -1) {
                perror("fork failed");
                exit(EXIT_FAILURE);
            }
            //child process
            if (c_pid == 0) {
                dup2(compile_err[1], STDERR_FILENO);
                close(compile_err[0]);
                close(compile_err[1]);

                //replaces the current process with a new one
                int status_code = execvp(command, args);

                std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
                _exit(1);
            }
            else {
                int status;
                //wait for child process result
                pid_t result = waitpid(c_pid, &status, 0);

                close(compile_err[1]);
                char ebuf[4096];
                ssize_t n = read(compile_err[0], ebuf, sizeof(ebuf));
                std::string compile_error(n > 0 ? std::string(ebuf, n) : "");
                close(compile_err[0]);

                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    throw std::runtime_error(compile_error);
                }
            }
            return binary_path;
        } else if (language == "java") {
            char* args[] = {
                (char*)"javac",
                (char*)"-d",
                (char*)"/tmp",
                (char*)source_path.c_str(),
                NULL
            };
            const char* command = "javac";

            int compile_err[2];
            pipe(compile_err);

            pid_t c_pid = fork();

            if (c_pid == -1) {
                perror("fork failed");
                exit(EXIT_FAILURE);
            }
            //child process
            if (c_pid == 0) {
                dup2(compile_err[1], STDERR_FILENO);
                close(compile_err[0]);
                close(compile_err[1]);

                //replaces the current process with a new one
                int status_code = execvp(command, args);

                std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
                _exit(1);
            }
            else {
                int status;
                //wait for child process result
                pid_t result = waitpid(c_pid, &status, 0);

                close(compile_err[1]);
                char ebuf[4096];
                ssize_t n = read(compile_err[0], ebuf, sizeof(ebuf));
                std::string compile_error(n > 0 ? std::string(ebuf, n) : "");
                close(compile_err[0]);

                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    throw std::runtime_error(compile_error);
                }
            }
            return "/tmp";
        }
        throw std::runtime_error("Unsupported language: " + language);
    }

    RunResult run(const std::string& binary_path, int job_id, const std::string& language, const std::string& stdin_input) {
        const int STACK_SIZE = 1024 * 1024;
        char* stack = new char[STACK_SIZE];

        int outpipe[2];
        int errpipe[2];
        int stdinpipe[2];
        if (pipe(outpipe) == -1 || pipe(errpipe) == -1 || pipe(stdinpipe) == -1) {
            perror("pipe() failed");
        }

        ChildArgs args;
        args.binary_path = binary_path;
        args.language = language;
        args.job_id = job_id;
        args.outpipe[0] = outpipe[0];
        args.outpipe[1] = outpipe[1];
        args.errpipe[0] = errpipe[0];
        args.errpipe[1] = errpipe[1];
        args.stdinpipe[0] = stdinpipe[0];
        args.stdinpipe[1] = stdinpipe[1];

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

            // write stdin input to child then close so child gets EOF
            if (!stdin_input.empty()) {
                write(stdinpipe[1], stdin_input.c_str(), stdin_input.size());
            }
            close(stdinpipe[1]);  // always close - sends EOF to child
            close(stdinpipe[0]);  // parent doesn't read from stdin pipe

            int status;
            long long ms;
            auto start = std::chrono::steady_clock::now();

            while (true) {
                //suspend parent until child changes state
                pid_t result = waitpid(pid, &status, WNOHANG);
                if (result != 0) break; //child exited
                auto elapsed = std::chrono::steady_clock::now() - start;
                ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                if (elapsed >= std::chrono::seconds(5)) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    delete[] stack;
                    return RunResult{"", "Timeout: execution exceeded 5 seconds", -1, ms};
                }
                usleep(10000);
            }

            if (check_oom(job_id)) {
                cleanup_cgroup(job_id);
                delete[] stack;
                return RunResult{"", "Memory Limit Exceeded", -1, ms};
            }

            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                if (sig == SIGSEGV) {
                    cleanup_cgroup(job_id);
                    delete[] stack;
                    return RunResult{"", "Memory limit exceeded", -1, ms};
                }
                cleanup_cgroup(job_id);
                delete[] stack;
                return RunResult{"", "Process killed by signal: " + std::to_string(sig), -1, ms};
            }

            std::string out_str, err_str;
            char buf[4096];
            ssize_t n;
            while ((n = read(outpipe[0], buf, sizeof(buf))) > 0)
                out_str.append(buf, n);
            while ((n = read(errpipe[0], buf, sizeof(buf))) > 0)
                err_str.append(buf, n);

            if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                if (sig == SIGSEGV) return RunResult{"", "Memory limit exceeded", -1, ms};
                return RunResult{"", "Process killed by signal: " + std::to_string(sig), -1, ms};
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                return RunResult{"", err_str, WEXITSTATUS(status), ms};
            }

            return RunResult{out_str, err_str, WEXITSTATUS(status), ms};
        }
    }
};