#!/usr/bin/env python3
"""Check host diagnostics when a guest child has no standard streams."""

import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_diagnostic_log.py wibo fixture")
    executable, fixture = map(lambda value: str(Path(value).resolve()), sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="wibo-diagnostics-") as directory:
        log_path = Path(directory) / "diagnostic.log"
        seed = b"diagnostic append sentinel\n"
        log_path.write_bytes(seed)
        environment = os.environ.copy()
        environment["WIBO_DIAGNOSTIC_LOG"] = str(log_path)
        environment["WIBO_DEBUG"] = "1"
        process = subprocess.Popen(
            [executable, fixture, "wibo"],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            output, errors = process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, errors = process.communicate(timeout=5)
            sys.stderr.buffer.write(output + errors)
            raise AssertionError("fixture timed out; owned process group terminated") from None
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        if process.returncode:
            sys.stderr.buffer.write(output + errors)
            raise AssertionError(f"fixture exited {process.returncode}")
        data = log_path.read_bytes()
        if not data.startswith(seed):
            raise AssertionError("diagnostics overwrote existing contents")
        missing = b"wibo: call reached missing import GetDateFormatA from kernel32\n"
        if data.count(missing) != 1:
            raise AssertionError("missing-import diagnostic was not recorded exactly once")
        for selector in (-10, -11, -12):
            if f"GetStdHandle({selector}) -> 0x0".encode() not in data:
                raise AssertionError("guest child standard handle changed")
        if missing in errors:
            raise AssertionError("configured diagnostics used guest stderr")
        if b"code=127 expected=127 result=1" not in output:
            raise AssertionError("missing child exit was not observed by parent")
    print("diagnostic log: append, NULL streams, worker exit passed")


if __name__ == "__main__":
    main()
