#include <mutex>

class JobManager {
private:
    std::vector<Job> jobs;
    int next_id;
    std::mutex mtx;

public:
    JobManager() {
        next_id = 1;
        jobs.reserve(100);
    }

    int submit(const std::string& code, const std::string& language) {
        std::lock_guard<std::mutex> lock(mtx);
        Job j = Job(next_id, code, language);
        jobs.push_back(j);
        next_id++;
        return next_id - 1;
    }

    Job& get(int id) {
        std::lock_guard<std::mutex> lock(mtx);
        for (Job& job : jobs) {
            if (job.id == id) return job;
        }
        throw std::runtime_error("No Job with that ID exists");
    }

    void set_status(int id, JobStatus new_status) {
        std::lock_guard<std::mutex> lock(mtx);
        for (Job& job : jobs) {
            if (job.id == id) {
                job.status = new_status;
                return;
            }
        }
    }

    const std::vector<Job>& all() const {
        return jobs;
    }
};