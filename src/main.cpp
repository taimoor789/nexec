#include <iostream>
#include "thread_pool.h"

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"')       out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

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

    std::ifstream ifs(source);
    if (!ifs.is_open()) {
        exit(1);
    }

    std::string source_code;
    std::string line;
    while (std::getline(ifs, line)) {
        source_code += line + "\n";
    }
    Executor executor;
    Job job(job_id, source_code, language);

    try {
        std::string source_path = executor.write_source(job);
        std::string binary_path = executor.compile(source_path, job_id, language);
        RunResult result = executor.run(binary_path, job_id, language);

        std::string output = escape(result.output);
        std::string error = escape(result.error);

        std::cout << "{\"output\": \"" << output << "\", \"error\": \"" << error
          << "\", \"exit_code\": " << result.exit_code
          << ", \"duration_ms\": " << result.duration_ms << "}";

    } catch (const std::exception& e) {
        std::cout << "{\"output\": \"" << "" <<
            "\", \"error\": \"" << escape(e.what()) <<
            ", \"duration_ms\": " << 0 << "}"
        << std::endl;
        return 1;
    }
    return 0;
}
