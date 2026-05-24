from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from pydantic import BaseModel
import subprocess
import json
import threading

app = FastAPI()

job_counter = 0
counter_lock = threading.Lock()

class SubmitRequest(BaseModel):
    source_code: str
    language: str

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





