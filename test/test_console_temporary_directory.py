import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

runner, fixture = map(lambda value: str(Path(value).resolve()), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="wibo-console-test-") as directory:
    root = Path(directory).resolve()
    profile = root / "boundary.sb"
    profile.write_text('(version 1)\n(allow default)\n(deny file-write* (require-all (require-not (subpath "' + str(root) + '")) (require-not (literal "/dev/null"))))\n')
    for suffix in ("", "/"):
        environment = os.environ | {"TMPDIR": str(root) + suffix}
        process = subprocess.Popen(["/usr/bin/sandbox-exec", "-f", str(profile), runner, fixture], env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            output, _ = process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)
            raise AssertionError("sandboxed console test timed out") from None
        if process.returncode:
            sys.stderr.buffer.write(output)
            raise AssertionError("console could not initialize in its configured temporary directory")
        if list(root.glob("wibo-console-*")):
            raise AssertionError("console scratch file was not unlinked")
