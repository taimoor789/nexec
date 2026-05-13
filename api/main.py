from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
import subprocess
import json

app = FastAPI()

class SubmitRequest(BaseModel):
    source_code: str
    language: str

@app.post("/submit")
def submit(request: SubmitRequest):
    job_id = 1
    extensions = {"cpp": ".cpp", "python": ".py", "java": ".java"}
    temp_file = f"/tmp/nexec_{job_id}{extensions[request.language]}"

    with open(temp_file, "w") as f:
        f.write(request.source_code)

    result = subprocess.run(
        ["./nexec", "--language", request.language, "--job-id", job_id, "--source", temp_file],
        capture_output=True,
        text=True
    )

    if result.returncode != 0:
        raise HTTPException(status_code=result.returncode)

    data = json.loads(result.stdout)
    return data





