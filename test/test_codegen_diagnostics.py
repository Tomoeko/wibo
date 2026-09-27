import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


GENERATOR = Path(__file__).resolve().parents[1] / "tools" / "gen_trampolines.py"
PROTOTYPE = """
namespace fixture {
__attribute__((annotate("CC:stdcall"), ms_abi)) int Probe(unsigned long long value);
}
"""


class CodegenDiagnosticsTests(unittest.TestCase):
    def generate(self, directory, source, extra_args=()):
        header = directory / "fixture.h"
        header.write_text(source)
        return subprocess.run(
            [
                sys.executable,
                str(GENERATOR),
                "--dll", "fixture",
                "--namespace", "fixture",
                "--arch", "x86_64",
                "--guest-arch", "x86_64",
                "--headers", str(header),
                "--out-asm", str(directory / "fixture.S"),
                "--out-hdr", str(directory / "fixture_trampolines.h"),
                *extra_args,
            ],
            capture_output=True,
            text=True,
            timeout=30,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )

    def test_missing_include_creates_no_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.generate(directory, PROTOTYPE + '#include "missing_fixture_header.h"\n')
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing_fixture_header.h", result.stderr)
            self.assertIn("Cannot generate trampolines", result.stderr)
            self.assertFalse((directory / "fixture.S").exists())
            self.assertFalse((directory / "fixture_trampolines.h").exists())

    def test_parse_error_preserves_previous_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            outputs = [directory / "fixture.S", directory / "fixture_trampolines.h"]
            for output in outputs:
                output.write_text("previous output\n")
            result = self.generate(directory, PROTOTYPE + "using Broken = ;\n")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Cannot generate trampolines", result.stderr)
            for output in outputs:
                self.assertEqual(output.read_text(), "previous output\n")

    def test_concepts_and_warning_generate_with_extra_include_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            includes = directory / "includes"
            includes.mkdir()
            (includes / "fixture_extension.h").write_text(
                "template<class T> concept Sized = sizeof(T) > 0;\n"
                "static_assert(Sized<int>);\n"
            )
            result = self.generate(
                directory,
                '#include "fixture_extension.h"\n#warning synthetic parser warning\n' + PROTOTYPE,
                ["--clang-arg=-I", "--clang-arg=" + str(includes)],
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("synthetic parser warning", result.stderr)
            self.assertTrue((directory / "fixture.S").exists())
            self.assertIn("Probe", (directory / "fixture_trampolines.h").read_text())


if __name__ == "__main__":
    unittest.main()
