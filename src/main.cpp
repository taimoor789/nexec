#include <iostream>
#include "job_manager.h"
#include "executor.h"

int main() {
    JobManager manager;
    Executor executor;

    int id = manager.submit("int main() { return 0; }", "cpp");
    Job& job = manager.get(id);

    std::string source_path = executor.write_source(job);
    std::cout << "Source written to: " << source_path << std::endl;

    std::string binary_path = executor.compile(source_path, id);
    std::cout << "Compiled to: " << binary_path << std::endl;

    return 0;
}