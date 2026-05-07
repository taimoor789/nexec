#include <iostream>
#include "thread_pool.h"

int main() {
    JobManager manager;
    Executor executor;
    ThreadPool pool(manager, executor);

    pool.start();

    for (int i = 1; i < 9; i++) {
        int id = manager.submit(
            "#include <iostream>\nint main() { std::cout << \"hello from job " + std::to_string(i) + "\" << std::endl; return 0; }",
            "cpp"
        );
        pool.submit(id);
    }

    std::this_thread::sleep_for(std::chrono::seconds(15));
    pool.stop();

    for (const Job& job : manager.all()) {
        std::cout << "Job " << job.id << ": " << job.output << std::endl;
    }

    return 0;
}