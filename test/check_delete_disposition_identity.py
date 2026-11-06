"""Check that closing a pending deletion cannot remove a known replacement."""
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix="wibo-disposition-") as directory:
        root = Path(directory)
        name = root / "current.bin"
        retired = root / "retired.bin"
        name.write_bytes(b"original file")
        windows_name = "Z:" + str(name).replace("/", "\\")
        process = subprocess.Popen(
            [*sys.argv[1:], windows_name], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=15):
                    raise AssertionError("guest did not reach the deletion barrier")
                if process.stdout.readline(32) not in (b"ready\n", b"ready\r\n"):
                    raise AssertionError("unexpected deletion barrier response")
            name.rename(retired)
            name.write_bytes(b"replacement file")
            output, errors = process.communicate(input=b"\n", timeout=10)
            if process.returncode:
                raise AssertionError(f"guest exit {process.returncode}: {errors.decode(errors='replace')}")
            if output:
                raise AssertionError("unexpected guest output after the barrier")
            if name.read_bytes() != b"replacement file":
                raise AssertionError("replacement file was changed")
            if retired.exists() and retired.read_bytes() != b"original file":
                raise AssertionError("renamed original file was changed")
            print("replacement file survived closing the original handle")
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=5)


if __name__ == "__main__":
    main()
