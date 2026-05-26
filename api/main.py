import os
from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from pydantic import BaseModel
import subprocess
import json
import threading
from typing import List
import anthropic

app = FastAPI()
client = anthropic.Anthropic(api_key=os.environ.get("ANTHROPIC_API_KEY"))

job_counter = 0
counter_lock = threading.Lock()

class SubmitRequest(BaseModel):
    source_code: str
    language: str

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

@app.get("/")
def index():
    return FileResponse("index.html")

@app.post("/submit")
def submit(request: SubmitRequest):
    global job_counter
    with counter_lock:
        job_counter += 1
        job_id = job_counter

    extensions = {"cpp": ".cpp", "python": ".py", "java": ".java"}
    temp_file = f"/tmp/nexec_{job_id}{extensions[request.language]}"

    print(f"DEBUG source:\n{request.source_code}", flush=True)

    with open(temp_file, "w") as f:
        f.write(request.source_code)

    result = subprocess.run(
        ["/nexec/nexec", "--language", request.language, "--job-id", str(job_id), "--source", temp_file],
        capture_output=True,
        text=True
    )

    data = json.loads(result.stdout)
    return data

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
        model = "claude-sonnet-4-6",
        max_tokens=1024,
        system=system_prompt,
        messages=messages
    )

    hint = response.content[0].text
    return {"hint": hint}


















