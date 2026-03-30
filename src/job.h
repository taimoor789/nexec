#pragma once
#include <string>

enum class JobStatus {
    Pending,
    Running,
    Done,
    Failed
};

struct Job {
    int id;
    std::string source_code;
    std::string language;
    std::string output;
    std::string error;
    JobStatus status;
    int exit_code;

    //Constructor so there's never an uninitialized job
    Job(int id, const std::string& code, const std::string& lang)
        : id(id),
          source_code(code),
          language(lang),
          output(""),
          error(""),
          status(JobStatus::Pending),
          exit_code(-1) {}
};
