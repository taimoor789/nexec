#include <iostream>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sched.h>
#include <cstring>
#include <signal.h>
#include <seccomp.h>
#include <chrono>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <termios.h>
#include <errno.h>
#include <vector>
#include <string>

void create_cgroup(int job_id) {
    std::ofstream subtree("/sys/fs/cgroup/cgroup.subtree_control");
    if (subtree) {subtree << "+memory"; subtree.close();}

    namespace fs = std::filesystem;
    fs::path cgroup_path = "/sys/fs/cgroup/nexec_" + std::to_string(job_id);
    try {
        fs::create_directory(cgroup_path);
    } catch (...) {
        return;
    }

    std::ofstream ofs(cgroup_path / "memory.max");
    if (ofs) {ofs << 256000000; ofs.close();}
}

void add_to_cgroup(int job_id, pid_t pid) {
    std::ofstream ofs("/sys/fs/cgroup/nexec_" + std::to_string(job_id) + "/cgroup.procs");
    if (ofs.is_open()) { ofs << std::to_string(pid); ofs.close(); }
}

bool check_oom(int job_id) {
    std::ifstream ifs("/sys/fs/cgroup/nexec_" + std::to_string(job_id) + "/memory.events");
    if (!ifs.is_open()) return false;
    std::string key; int value;
    while (ifs >> key >> value) if (key == "oom_kill" && value > 0) return true;
    return false;
}

void cleanup_cgroup(int job_id) {
    rmdir(("/sys/fs/cgroup/nexec_" + std::to_string(job_id)).c_str());
}

struct ChildArgs {
    std::vector<char*> exec_argv;
    std::string language;
};

int child_fn(void* arg) {
    ChildArgs* args = static_cast<ChildArgs*>(arg);

    setsid();
    ioctl(STDIN_FILENO, TIOCSCTTY, 0);

    mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);

    struct rlimit cpu_limit;
    cpu_limit.rlim_cur = 10;
    cpu_limit.rlim_max = 10;
    setrlimit(RLIMIT_CPU, &cpu_limit);

    if (args->language != "java") {
        scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(connect), 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(ptrace), 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(mount), 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(umount2), 0);

        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(fork), 0);
        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(vfork), 0);

        seccomp_rule_add(ctx, SCMP_ACT_ERRNO(ENOSYS), SCMP_SYS(clone3), 0);

        seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(clone), 1,
            SCMP_A0(SCMP_CMP_MASKED_EQ, CLONE_THREAD, 0));

        seccomp_load(ctx);
        seccomp_release(ctx);
    }

    if (args->language == "cpp") {
        struct rlimit mem_limit;
        mem_limit.rlim_cur = 256 * 1024 * 1024;
        mem_limit.rlim_max = 256 * 1024 * 1024;
        setrlimit(RLIMIT_AS, &mem_limit);
    }

    execvp(args->exec_argv[0], args->exec_argv.data());
    std::cerr << "execvp() failed: " << strerror(errno) << std::endl;
    _exit(1);
}

pid_t g_child_pid = -1;
int g_job_id = -1;

void handle_sigterm(int) {
    if (g_child_pid > 0) kill(g_child_pid, SIGKILL);
    if (g_job_id >= 0) cleanup_cgroup(g_job_id);
    _exit(143);
}

int main(int argc, char* argv[]) {
    std::string language, id;
    int i = 1;
    for (; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--language" && i + 1 < argc) language = argv[++i];
        else if (arg == "--job-id" && i + 1 < argc) id = argv[++i];
        else if (arg == "--") { i++; break; }
    }
    if (language.empty() || id.empty()) {
        std::cerr << "missing --language or --job-id" << std::endl;
        return 1;
    }
    int job_id = std::stoi(id);
    g_job_id = job_id;

    std::vector<char*> exec_argv;
    for (; i < argc; i++) exec_argv.push_back(argv[i]);
    exec_argv.push_back(nullptr);
    if (exec_argv.size() <= 1) {
        std::cerr << "no command given after --" << std::endl;
        return 1;
    }

    signal(SIGTERM, handle_sigterm);

    create_cgroup(job_id);

    ChildArgs cargs;
    cargs.exec_argv = exec_argv;
    cargs.language = language;

    const int STACK_SIZE = 1024 * 1024;
    char* stack = new char[STACK_SIZE];

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                       CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, &cargs);
    if (pid == -1) {
        perror("clone failed");
        delete[] stack;
        return 1;
    }
    g_child_pid = pid;

    add_to_cgroup(job_id, pid);

    int status;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result != 0) break;
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed >= std::chrono::minutes(5)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            cleanup_cgroup(job_id);
            delete[] stack;
            std::cerr << "Session exceeded 5 minute limit" << std::endl;
            return 124;
        }
        usleep(20000);
    }

    bool oom = check_oom(job_id);
    cleanup_cgroup(job_id);
    delete[] stack;

    if (oom) {
        std::cerr << "Memory limit exceeded" << std::endl;
        return 137;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGXCPU) std::cerr << "CPU time limit exceeded" << std::endl;
        else std::cerr << "Process killed by signal: " << sig << std::endl;
        return 128 + sig;
    }
    return WEXITSTATUS(status);
}

