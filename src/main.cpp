#include <iostream>
#include "job_manager.h"

int main() {
    JobManager manager;

    int id1 = manager.submit("print('hello')", "python");
    int id2 = manager.submit("print('world')", "python");

    std::cout << "Submitted job " << id1 << " and job " << id2 << std::endl;

    manager.set_status(id1, JobStatus::Running);
    std::cout << "Job 1 status is now: Running" << std::endl;

    return 0;
}