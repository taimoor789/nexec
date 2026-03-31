#include <iostream>
#include <fstream>
#include "job.h"
#include <format>
#include <unistd.h>
#include <sys/wait.h>


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
         if (c_pid == 0) {
            int status_code = execvp(command, args);

            std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
            _exit(1);
        }
        else {
            int status;
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

};