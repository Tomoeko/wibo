#!/usr/bin/env python3
"""Check that diagnostic output remains usable across thread suspension."""

import argparse
import io
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def report_failure(output, errors):
    control_trace = bytearray()
    for line in io.BytesIO(errors):
        if b"GetCurrentThreadId()" in line or b"debug messages omitted" in line:
            continue
        control_trace.extend(line)
        if len(control_trace) > 16384:
            del control_trace[:-16384]
    sys.stderr.buffer.write(output)
    if control_trace:
        sys.stderr.buffer.write(b"fixture control trace:\n" + control_trace + b"\n")
    sys.stderr.buffer.write(b"fixture stderr tail:\n" + errors[-4096:])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wibo")
    parser.add_argument("fixture")
    parser.add_argument("--context-unavailable", action="store_true")
    parser.add_argument("--stderr", action="store_true")
    arguments = parser.parse_args()
    command = [str(Path(arguments.wibo).resolve()), "-D", str(Path(arguments.fixture).resolve())]
    if arguments.context_unavailable:
        command.append("--context-unavailable")
    with tempfile.TemporaryDirectory(prefix="wibo-suspension-") as directory:
        log_path = Path(directory) / "diagnostic.log"
        seed = b"diagnostic append sentinel\n"
        environment = os.environ.copy()
        environment.pop("WIBO_DIAGNOSTIC_LOG", None)
        if not arguments.stderr:
            log_path.write_bytes(seed)
            environment["WIBO_DIAGNOSTIC_LOG"] = str(log_path)
        process = subprocess.Popen(
            command,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True,
        )
        try:
            output, errors = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, errors = process.communicate(timeout=5)
            report_failure(output, errors)
            raise AssertionError("suspension fixture timed out; owned process group terminated") from None

        def fail(message):
            report_failure(output, errors)
            raise AssertionError(message)

        if process.returncode:
            fail(f"suspension fixture exited {process.returncode}")
        normalized_output = output.replace(b"\r\n", b"\n")
        if (
            b"completed=256 target=256" not in normalized_output
            or b"software_checks_failed=0\n" not in normalized_output
        ):
            fail("suspension cycles did not complete")
        unavailable = 256 if arguments.context_unavailable else 0
        if f"context_unavailable={unavailable}\n".encode() not in normalized_output:
            fail("context disposition was not explicit")
        if arguments.stderr:
            data = errors
        else:
            data = log_path.read_bytes()
            if not data.startswith(seed):
                fail("diagnostics overwrote existing contents")
            if b"GetCurrentThreadId()" in errors:
                fail("configured diagnostics used standard error")
        for operation in (b"GetCurrentThreadId()", b"SuspendThread(", b"GetThreadContext(", b"ResumeThread("):
            if operation not in data:
                fail(f"missing diagnostic operation {operation.decode()}")
    print("diagnostic suspension: 256 cycles completed")


if __name__ == "__main__":
    main()
