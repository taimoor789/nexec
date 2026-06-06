from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from pydantic import BaseModel
from typing import List
import subprocess
import json
import threading
import anthropic
import os
import asyncio
import pty
import fcntl
import termios
import struct

app = FastAPI()

job_counter = 0
counter_lock = threading.Lock()

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
        return ["/usr/bin/python3", "-u", binary_path]
    elif language == "java":
        class_name = f"nexec_{job_id}"
        return ["/usr/bin/java", "-cp", "/tmp", class_name]
    else:
        return [binary_path]

def set_nonblocking(fd: int):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

@app.get("/")
def index():
    return FileResponse("index.html")

@app.websocket("/ws/run")
async def run_ws(websocket: WebSocket):
    await websocket.accept()

    try:
        #receive job details
        msg = await websocket.receive_json()
        source_code = msg["source_code"]
        language = msg["language"]
        job_id = get_job_id()

        #write source
        source_file = write_source(source_code, language, job_id)

        #compile
        success, binary_path, compile_error = compile_source(source_file, language, job_id)
        if not success:
            await websocket.send_json({"type": "compile_error", "data": compile_error})
            await websocket.close()
            return

        await websocket.send_json({"type": "ready"})

        #create PTY
        master_fd, slave_fd = pty.openpty()

        #set terminal size
        winsize = struct.pack("HHHH", 50, 220, 0, 0)
        fcntl.ioctl(slave_fd, termios.TIOCSWINSZ, winsize)

        #spawn process with slave as its terminal
        cmd = build_exec_cmd(binary_path, language, job_id)
        proc = subprocess.Popen(
            cmd,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            close_fds=True,
            preexec_fn=os.setsid
        )

        # close slave in parent, only child needs it
        os.close(slave_fd)
        set_nonblocking(master_fd)

        loop = asyncio.get_event_loop()

        #stream output from PTY master to browser
        async def read_pty():
            try:
                while True:
                    try:
                        data = await loop.run_in_executor(None, lambda: _read_master(master_fd))
                        if data is None:
                            break
                        await websocket.send_json({"type": "output", "data": data.decode("utf-8", errors="replace")})
                    except OSError:
                        break
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

        #receive input from browser, write to PTY master
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
                        proc.kill()
                        break
            except (WebSocketDisconnect, Exception):
                pass

        #run both concurrently, stop when process exits
        read_task = asyncio.create_task(read_pty())
        write_task = asyncio.create_task(write_pty())

        #wait for process to finish
        exit_code = await loop.run_in_executor(None, proc.wait)

        # drain remaining output
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
            proc.kill()
        except Exception:
            pass
@app.get("/xterm.js")
def xterm_js(): return FileResponse("xterm.js")

@app.get("/xterm.css")
def xterm_css(): return FileResponse("xterm.css")

@app.get("/xterm-addon-fit.js")
def xterm_fit(): return FileResponse("xterm-addon-fit.js")

@app.get("/favicon.ico")
def favicon(): return FileResponse("favicon.svg")

@app.post("/explain")
def explain(request: ExplainRequest):
    system_prompt = """You are a CS teaching assistant helping university students debug and understand their code.

Your rules:
- Never give the corrected code or the direct answer
- Ask one focused leading question at a time
- Point to the specific line or concept that needs attention
- Explain the concept behind the error, not just what the fix is
- If the code runs but the output seems wrong given the student's goal, question whether the logic matches their intent
- Keep responses concise — one insight per reply
- If there are no issues at all, say so clearly and briefly"""

    user_message = f"""Language: {request.language}

Code:
{request.source_code}

Output: {request.output}
Error: {request.error}
Exit code: {request.exit_code}

What my code is supposed to do: {request.context if request.context else "not provided"}"""

    messages = [{"role": m.role, "content": m.content} for m in request.message_history]
    messages.append({"role": "user", "content": user_message})

    response = client.messages.create(
        model="claude-sonnet-4-6",
        max_tokens=1024,
        system=system_prompt,
        messages=messages
    )

    hint = response.content[0].text
    return {"hint": hint}