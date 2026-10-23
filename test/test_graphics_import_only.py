import os
from pathlib import Path
import signal
import subprocess
import sys

runner, fixture = map(lambda value: str(Path(value).resolve()), sys.argv[1:])
process = subprocess.Popen([runner, fixture, "--call"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
try:
    output, _ = process.communicate(timeout=15)
except subprocess.TimeoutExpired:
    os.killpg(process.pid, signal.SIGKILL)
    process.communicate(timeout=5)
    raise AssertionError("unsupported graphics call timed out") from None
if process.returncode != 127 or b"call reached missing import Direct3DCreate9 from d3d9" not in output:
    sys.stderr.buffer.write(output)
    raise AssertionError("graphics call did not fail through the missing-import path")
