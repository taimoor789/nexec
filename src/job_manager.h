#include <vector>

#include "job.h"

class JobManager {
private:
    std::vector<Job> jobs;
    int next_id;

public:
    JobManager() {
        next_id = 1;
    }

    int submit(const std::string& code, const std::string& language) {
        Job j = Job(next_id, code, language);
        jobs.push_back(j);
        next_id++;
        return next_id - 1;
    }

    Job& get(int id) {
       for (Job& job : jobs) {
           if (job.id == id) {
               return job;
           }
       }
        throw std::runtime_error("No Job with that ID exists");
    }

    void set_status(int id, JobStatus new_status) {
        Job& job = get(id);
        job.status = new_status;
    }

    const std::vector<Job>& all() const {
        return jobs;
    }

};