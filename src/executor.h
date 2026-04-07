#include <iostream>
#include <fstream>
#include "job.h"
#include <format>
#include <unistd.h>
#include <sys/wait.h>

struct RunResult {
    std::string output;
    std::string error;
    int exit_code;
};

class Executor {

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
        int outpipe[2];
        int errpipe[2];

        if (pipe(outpipe) == -1 || pipe(errpipe) == -1) {
            perror("pipe() failed");
        }

        pid_t pid = fork();

        if (pid == -1) {
            perror("fork failed");
            exit(EXIT_FAILURE);
        }

        if (pid == 0) {
            close(outpipe[0]);
            close(errpipe[0]);

            //stdout to outpipe's write end
            dup2(outpipe[1], STDOUT_FILENO);
            //stederr to errpipe's write end
            dup2(errpipe[1], STDERR_FILENO);

            close(outpipe[1]);
            close(errpipe[1]);

            char* args[] = {(char*)binary_path.c_str(), NULL};

            //replaces the current process with a new one
            int status = execvp(binary_path.c_str(), args);
            std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
            _exit(1);
        }
        //parent process
        else {
            close(outpipe[1]);
            close(errpipe[1]);

            char out_buffer[128];
            char err_buffer[128];
            ssize_t outCount = read(outpipe[0], out_buffer, sizeof(out_buffer));
            ssize_t errCount = read(errpipe[0], err_buffer, sizeof(err_buffer));

            if (outCount == -1 || errCount == -1) {
                perror("read() failed");
            }

            std::string out_str(out_buffer, outCount);
            std::string err_str(err_buffer, errCount);

            int status;
            pid_t result = waitpid(pid, &status, 0);

            if (result == -1) {
                throw std::system_error(errno, std::generic_category(), "waitpid failed");
            }

            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                throw std::runtime_error("Compilation failed");
            }

            return RunResult{out_str, err_str, WEXITSTATUS(status)};
        }
    }
};