from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel
from typing import List
import subprocess
import threading
import anthropic
import os
import asyncio
import pty
import fcntl
import termios
import struct
from pydantic import field_validator
from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.util import get_remote_address
from slowapi.errors import RateLimitExceeded
from collections import defaultdict
import time

app = FastAPI()
limiter = Limiter(key_func=get_remote_address)
app.state.limiter = limiter
app.add_exception_handler(RateLimitExceeded, _rate_limit_exceeded_handler)

job_counter = 0
counter_lock = threading.Lock()
ws_attempts = defaultdict(list)
ws_lock = threading.Lock()

client = anthropic.Anthropic(api_key=os.environ.get("ANTHROPIC_API_KEY"))

class Message(BaseModel):
    role: str
    content: str

class ExplainRequest(BaseModel):
    language: str
    source_code: str
    output: str
    error: str
    exit_code: int
    context: str
    message_history: List[Message]
    @field_validator('source_code')
    def limit_source_size(cls, v):
        if len(v) > 50000:
            raise ValueError('Source code too large')
        return v

def get_job_id() -> int:
    global job_counter
    with counter_lock:
        job_counter += 1
        return job_counter

def write_source(source_code: str, language: str, job_id: int) -> str:
    extensions = {"cpp": ".cpp", "python": ".py", "java": ".java"}
    temp_file = f"/tmp/nexec_{job_id}{extensions[language]}"
    if language == "java":
        class_name = f"nexec_{job_id}"
        source_code = source_code.replace("NEXEC_CLASS", class_name)
    with open(temp_file, "w") as f:
        f.write(source_code)
    return temp_file

def compile_source(source_file: str, language: str, job_id: int) -> tuple[bool, str, str]:
    """Returns (success, binary_path, error_message)"""
    if language == "python":
        return True, source_file, ""

    if language == "cpp":
        binary_path = f"/tmp/nexec_{job_id}"
        result = subprocess.run(
            ["g++", "-std=c++20", "-o", binary_path, source_file],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            return False, "", result.stderr
        return True, binary_path, ""

    if language == "java":
        result = subprocess.run(
            ["javac", "-d", "/tmp", source_file],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            return False, "", result.stderr
        return True, f"/tmp/nexec_{job_id}", ""

    return False, "", f"Unsupported language: {language}"

def build_exec_cmd(binary_path: str, language: str, job_id: int) -> list:
    if language == "python":
        real_cmd = ["/usr/bin/python3", "-u", binary_path]
    elif language == "java":
        class_name = f"nexec_{job_id}"
        real_cmd = ["/usr/bin/java", "-cp", "/tmp", class_name]
    else:
        real_cmd = [binary_path]

    return ["/nexec/sandbox_exec", "--language", language, "--job-id", str(job_id), "--"] + real_cmd

def set_nonblocking(fd: int):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

@app.get("/")
def index():
    return FileResponse("index.html")

def check_ws_rate_limit(ip: str, max_per_minute: int = 15) -> bool:
    now = time.time()
    with ws_lock:
        ws_attempts[ip] = [t for t in ws_attempts[ip] if now - t < 60]
        if len(ws_attempts[ip]) >= max_per_minute:
            return False
        ws_attempts[ip].append(now)
        return True

@app.websocket("/ws/run")
async def run_ws(websocket: WebSocket):
    origin = websocket.headers.get("origin", "")
    allowed = ["http://nexec.taimoorkiani.com:8000", "http://localhost:8000"]
    if origin not in allowed:
        await websocket.close(code=1008)
        return
    await websocket.accept()

    client_ip = websocket.client.host
    if not check_ws_rate_limit(client_ip):
        await websocket.close(code=1008)
        return

    proc = None
    loop = asyncio.get_event_loop()

    async def terminate_proc():
        if proc is not None:
            proc.terminate()
            try:
                await asyncio.wait_for(loop.run_in_executor(None, proc.wait), timeout=1.0)
            except asyncio.TimeoutError:
                proc.kill()

    try:
        msg = await websocket.receive_json()
        source_code = msg["source_code"]

        if len(source_code) > 50000:
            await websocket.send_json({"type": "error", "data": "Source code too large"})
            await websocket.close()
            return

        language = msg["language"]
        job_id = get_job_id()
        source_file = write_source(source_code, language, job_id)

        success, binary_path, compile_error = compile_source(source_file, language, job_id)
        if not success:
            await websocket.send_json({"type": "compile_error", "data": compile_error})
            await websocket.close()
            return

        await websocket.send_json({"type": "ready"})

        master_fd, slave_fd = pty.openpty()
        winsize = struct.pack("HHHH", 50, 220, 0, 0)
        fcntl.ioctl(slave_fd, termios.TIOCSWINSZ, winsize)

        cmd = build_exec_cmd(binary_path, language, job_id)
        proc = subprocess.Popen(
            cmd, stdin=slave_fd, stdout=slave_fd, stderr=slave_fd,
            close_fds=True, preexec_fn=os.setsid
        )
        os.close(slave_fd)
        set_nonblocking(master_fd)

        async def read_pty():
            try:
                while True:
                    data = await loop.run_in_executor(None, lambda: _read_master(master_fd))
                    if data is None:
                        break
                    await websocket.send_json({"type": "output", "data": data.decode("utf-8", errors="replace")})
            except WebSocketDisconnect:
                pass

        def _read_master(fd):
            import select
            r, _, _ = select.select([fd], [], [], 0.1)
            if r:
                try:
                    return os.read(fd, 4096)
                except OSError:
                    return None
            return b""

        async def write_pty():
            try:
                while True:
                    msg = await websocket.receive_json()
                    if msg.get("type") == "input":
                        os.write(master_fd, msg["data"].encode("utf-8"))
                    elif msg.get("type") == "resize":
                        ws = struct.pack("HHHH", msg["rows"], msg["cols"], 0, 0)
                        fcntl.ioctl(master_fd, termios.TIOCSWINSZ, ws)
                    elif msg.get("type") == "kill":
                        await terminate_proc()
                        break
            except (WebSocketDisconnect, Exception):
                pass

        read_task = asyncio.create_task(read_pty())
        write_task = asyncio.create_task(write_pty())

        exit_code = await loop.run_in_executor(None, proc.wait)

        await asyncio.sleep(0.15)
        read_task.cancel()
        write_task.cancel()

        try:
            os.close(master_fd)
        except OSError:
            pass

        await websocket.send_json({"type": "exit", "exit_code": exit_code})

    except WebSocketDisconnect:
        pass
    except Exception as e:
        try:
            await websocket.send_json({"type": "error", "data": str(e)})
        except Exception:
            pass
    finally:
        try:
            await terminate_proc()
        except Exception:
            pass
@app.get("/xterm.js")
def xterm_js(): return FileResponse("xterm.js")

@app.get("/xterm.css")
def xterm_css(): return FileResponse("xterm.css")

@app.get("/xterm-addon-fit.js")
def xterm_fit(): return FileResponse("xterm-addon-fit.js")

@app.get("/monaco-loader.js")
def monaco_loader(): return FileResponse("monaco-loader.js")

@app.get("/favicon.ico")
def favicon(): return FileResponse("favicon.svg")

@app.post("/explain")
@limiter.limit("5/minute")
async def explain(request: Request, body: ExplainRequest):
    system_prompt = """You are a CS teaching assistant helping university students debug and understand their code.

    Your rules:
    - Never give the corrected code or the direct answer
    - Ask one focused leading question at a time
    - Point to the specific line or concept that needs attention
    - Explain the concept behind the error, not just what the fix is
    - If the code runs but the output seems wrong given the student's goal, question whether the logic matches their intent
    - Keep responses concise — one insight per reply
    - If there are no issues at all, say so clearly and briefly"""

    user_message = f"""Language: {body.language}

    Code:
    {body.source_code}
    
    Output: {body.output}
    Error: {body.error}
    Exit code: {body.exit_code}
    
    What my code is supposed to do: {body.context if body.context else "not provided"}"""

    messages = [{"role": m.role, "content": m.content} for m in body.message_history]
    messages.append({"role": "user", "content": user_message})

    response = client.messages.create(
        model="claude-sonnet-4-6",
        max_tokens=1024,
        system=system_prompt,
        messages=messages
    )

    hint = response.content[0].text
    return {"hint": hint}