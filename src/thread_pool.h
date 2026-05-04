#include <queue>
#include <seccomp.h>
#include <thread>
#include "executor.h"
#include "job_manager.h"


class ThreadPool {
private:
    std::queue<int> job_ids;
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    bool running;
    JobManager job_manager;
    Executor executor;

public:
    void submit(int job_id) {
        job_ids.push(job_id);
        cv.notify_one();
    }

    int start() {

    }




}
