#include <iostream>
#include "thread_pool.h"

int main(int argc, char* argv[]) {
    std::string language;
    std::string id;
    std::string source;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--language" && i + 1 < argc) {
            language = argv[i + 1];
            i++;
        }

        if (arg == "--source" && i + 1 < argc) {
            source = argv[i + 1];
            i++;
        }

        if (arg == "--job-id" && i + 1 < argc) {
            id = argv[i + 1];
            i++;
        }
    }

    if (language.empty() || id.empty() || source.empty()) {
        exit(1);
    }

    int job_id = std::stoi(id);


}


//     JobManager manager;
//     Executor executor;
//     ThreadPool pool(manager, executor);
//
//     pool.start();
//
//     for (int i = 1; i < 9; i++) {
//         int id = manager.submit(
//     "public class NEXEC_CLASS {\n    public static void main(String[] args) {\n        System.out.println(\"hello from java!\");\n    }\n}",
//     "java"
//          );
//         pool.submit(id);
//     }
//
//     std::this_thread::sleep_for(std::chrono::seconds(15));
//     pool.stop();
//
//     for (const Job& job : manager.all()) {
//         std::cout << "Job " << job.id << ": " << job.output << std::endl;
//         std::cout << "Error: " << job.error << std::endl;
//     }
//
//     return 0;
// }