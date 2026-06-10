# Nexec

### What it is

Nexec is a sandboxed code execution engine that compiles and runs code in isolated Linux environments. Built as the backend infrastructure behind tools like LeetCode and HackerRank, then extended with an interactive terminal and a AI hint system aimed to help university students understand and learn from their code.

### What it does

You write code in the browser editor, hit Run, and your code executes inside an isolated Linux environment. Output streams back in real time through an interactive terminal. If you get stuck, clicking Explain sends your code and its actual output to Claude, which gives you a targeted hint without just handing you the answer.

### Why I built it

While mentoring students learning to code, I saw the same pattern of someone spending 20 minutes stuck on a bug they can't see, and the answer they eventually get (from a mentor or from pasting into ChatGPT) which doesn't actually teach them anything. I wanted a tool that runs the code for real, shows the actual error, and guides the student toward fixing it themselves.

### How it works

The execution engine is written in C++. It uses `clone()` with `CLONE_NEWPID` and `CLONE_NEWNS` to isolate each job in its own PID and mount namespace, `seccomp-BPF` to block dangerous syscalls like `socket()` and `fork()`, `setrlimit` to enforce a 256MB memory limit, and cgroup v2 for memory accounting. A thread pool with 4 workers processes jobs concurrently off a mutex protected queue.

The Python FastAPI layer sits in front of the engine. When you click Run, the browser opens a WebSocket connection to the server. Python compiles the source, creates a PTY with `os.openpty()`, and spawns the process with the PTY slave as its controlling terminal. Output from the PTY master streams over the WebSocket to xterm.js in the browser. Keystrokes from the terminal go back over the WebSocket and get written to the PTY master.

The AI hint system calls the Anthropic API with a system prompt that enforces the student to think for themselves. It receives the student's code, the actual runtime output, and the full conversation history from previous hints in the same session, so follow up hints are aware of what was already discussed.

```
Browser (Monaco + xterm.js)
    |
    | WebSocket /ws/run
    |
FastAPI (Python)
    |-- compile: g++ / javac / python
    |-- PTY: os.openpty()
    |-- stream: master fd <-> WebSocket
    |
    | POST /explain
    |
Anthropic API (Hints)
```

### Tech Stack

- C++ (execution engine)
- Python FastAPI (WebSocket execution flow, PTY management, Anthropic API integration)
- xterm.js  (interactive browser terminal)
- Docker

### Available languages 

- Python
- C++
- Java


### Running locally

Requires Docker and docker-compose. The C++ engine uses Linux namespaces and seccomp so it has to run inside a container, it won't compile on macOS directly.

Clone the repo:

```bash
git clone https://github.com/taimoor789/nexec.git
cd nexec
```

Create a `Dockerfile` in the project root:

```dockerfile
FROM ubuntu:22.04
RUN apt update && apt install -y g++ libseccomp-dev python3 python3-pip default-jdk
WORKDIR /nexec
COPY src/ ./src/
RUN g++ -std=c++20 -o nexec src/main.cpp -lseccomp
COPY api/ ./api/
RUN pip3 install fastapi "uvicorn[standard]" anthropic
WORKDIR /nexec/api
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
```

Create a `docker-compose.yml` in the project root, substituting your Anthropic API key:

```yaml
services:
  api:
    build: .
    privileged: true
    stdin_open: true
    ports:
      - "8000:8000"
    environment:
      - ANTHROPIC_API_KEY=your-key-here
```

Build and run:

```bash
docker-compose up --build
```

Open `http://localhost:8000`.

### Notes

The sandboxing (namespaces, seccomp, cgroups) is implemented and verified working on real Linux. On Docker Desktop for macOS, cgroup delegation is restricted so cgroup memory limits silently and `setrlimit` handles memory limiting in that environment instead. On a real Linux deployment, both layers are active.
