#pragma once
#include <queue>
#include <seccomp.h>
#include <thread>
#include "executor.h"
#include "job_manager.h"
#include <condition_variable>
#include <mutex>

class ThreadPool {
private:
    std::queue<int> job_ids;
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable cv;
    bool running = false;
    JobManager& job_manager;
    Executor& executor;

    void worker_loop() {
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [this] { return !job_ids.empty() || !running; });
            if (!running && job_ids.empty()) {
                break;
            }
            int job_id = job_ids.front();
            job_ids.pop();

            lock.unlock();

            try {
                Job& job = job_manager.get(job_id);
                std::string source_path = executor.write_source(job);
                std::string binary_path = executor.compile(source_path, job_id);
                RunResult result = executor.run(binary_path, job_id);

                job.output = result.output;
                job.error = result.error;
                job.exit_code = result.exit_code;
                job_manager.set_status(job_id, JobStatus::Done);
            } catch (const std::exception& e) {
                job_manager.set_status(job_id, JobStatus::Failed);
            }
        }
    }


public:
    ThreadPool(JobManager& job_manager, Executor& executor) :
    job_manager(job_manager),
    executor(executor) {}


    void submit(int job_id) {
        std::lock_guard<std::mutex> lock(mutex);
        job_ids.push(job_id);
        cv.notify_one();
    }

    void start() {
        running = true;
        for (int i = 0; i < 4; i++) {
            threads.emplace_back(&ThreadPool::worker_loop, this);
        }
    }

    void stop() {
        running = false;
        cv.notify_all();
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
};
