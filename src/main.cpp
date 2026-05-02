#include <iostream>
#include "job_manager.h"
#include "executor.h"

int main() {
    JobManager manager;
    Executor executor;

    int id = manager.submit(
    "#include <iostream>\nint main() { std::cout << \"hello from nexec!\" << std::endl; return 0; }",
    "cpp"
    );

    Job& job = manager.get(id);

    std::string source_path = executor.write_source(job);
    std::string binary_path = executor.compile(source_path, id);
    RunResult result = executor.run(binary_path, id);

    std::cout << "Output: " << result.output << std::endl;
    std::cout << "Exit code: " << result.exit_code << std::endl;
    std::cout << "Error: " << result.error << std::endl;

    return 0;
}