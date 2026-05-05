cmake_minimum_required(VERSION 4.1)
project(nexec)

set(CMAKE_CXX_STANDARD 20)

add_executable(nexec
        src/main.cpp
        src/job.h
        src/executor.h
        src/job_manager.h
        src/thread_pool.h)
