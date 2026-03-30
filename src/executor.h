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
        char* args[] = {(char*)"executor.h", NULL};
        const char* command = "g++";

        pid_t c_pid = fork();
        if (c_pid == -1) {
            perror("fork");
            exit(EXIT_FAILURE);
        }
        else if (c_pid > 0) {
            std::cout << "print from parent" << getpid() << std::endl;
        }
        else {
            std::cout << "print from child" << getpid() << std::endl;
        }
        return 0;
    }

};