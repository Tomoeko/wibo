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
DIRECT_ENTRY_PROTOTYPE = """
namespace fixture {
struct Record { unsigned long long value; };
__attribute__((annotate("CC:stdcall"), annotate("GUEST_ENTRY:fixtureDirectEntry"), ms_abi))
unsigned long long Probe(Record *value);
}
"""
STACK_VARARGS_PROTOTYPE = """
namespace fixture {
__attribute__((annotate("CC:cdecl"), annotate("GUEST_STACK_VARARGS")))
int Probe(void *buffer, const unsigned short *format, const void *arguments);
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

    def test_generation_preserves_unchanged_output_timestamps(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.generate(directory, PROTOTYPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            assembly = directory / "fixture.S"
            header = directory / "fixture_trampolines.h"
            outputs = [assembly, header]
            original = {output: output.read_bytes() for output in outputs}
            for output in outputs:
                os.utime(output, ns=(1_600_000_000_000_000_000, 1_600_000_000_000_000_000))
            timestamps = {output: output.stat().st_mtime_ns for output in outputs}

            result = self.generate(directory, PROTOTYPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            for output in outputs:
                self.assertEqual(output.read_bytes(), original[output])
                self.assertEqual(output.stat().st_mtime_ns, timestamps[output])

            header.unlink()
            result = self.generate(directory, PROTOTYPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(header.read_bytes(), original[header])
            self.assertEqual(assembly.stat().st_mtime_ns, timestamps[assembly])

            os.utime(header, ns=(timestamps[header], timestamps[header]))
            result = self.generate(directory, PROTOTYPE.replace("Probe", "ReplacementProbe"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("ReplacementProbe", header.read_text())
            self.assertNotEqual(header.read_bytes(), original[header])
            self.assertNotEqual(header.stat().st_mtime_ns, timestamps[header])
            self.assertEqual(assembly.read_bytes(), original[assembly])
            self.assertEqual(assembly.stat().st_mtime_ns, timestamps[assembly])

            header_timestamp = header.stat().st_mtime_ns
            assembly.write_text("stale assembly\n")
            os.utime(assembly, ns=(timestamps[assembly], timestamps[assembly]))
            result = self.generate(directory, PROTOTYPE.replace("Probe", "ReplacementProbe"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(assembly.read_bytes(), original[assembly])
            self.assertNotEqual(assembly.stat().st_mtime_ns, timestamps[assembly])
            self.assertEqual(header.stat().st_mtime_ns, header_timestamp)

    def test_external_guest_entry_preserves_guest_registers(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.generate(directory, DIRECT_ENTRY_PROTOTYPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            mapping = (directory / "fixture_trampolines.h").read_text()
            self.assertIn('extern "C" unsigned long long __attribute__((ms_abi)) fixtureDirectEntry(', mapping)
            self.assertIn('return (void*)&fixture::fixtureDirectEntry;', mapping)
            self.assertNotIn("wibo_guest_to_host_fixture_Probe", mapping)

            result = self.generate(directory, DIRECT_ENTRY_PROTOTYPE, ["--guest-arch", "x86"])
            self.assertEqual(result.returncode, 0, result.stderr)
            outputs = [directory / "fixture.S", directory / "fixture_trampolines.h"]
            annotated = [output.read_bytes() for output in outputs]
            assembly = outputs[0].read_text()
            self.assertIn(".code32\n\tjmp SYMBOL_NAME(fixtureDirectEntry)", assembly)
            self.assertNotIn("LJMP64", assembly)
            self.assertNotIn("push ebp", assembly)
            source = DIRECT_ENTRY_PROTOTYPE.replace('annotate("GUEST_ENTRY:fixtureDirectEntry"), ', "")
            result = self.generate(directory, source, ["--guest-arch", "x86"])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(outputs[1].read_bytes(), annotated[1])
            self.assertIn("LJMP64", outputs[0].read_text())
            self.assertNotEqual(outputs[0].read_bytes(), annotated[0])

            result = self.generate(directory, DIRECT_ENTRY_PROTOTYPE,
                                   ["--arch", "x86", "--guest-arch", "x86"])
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(".code32\n\tjmp SYMBOL_NAME(fixtureDirectEntry)", outputs[0].read_text())
            self.assertNotIn("call", outputs[0].read_text())

    def test_guest_entry_annotation_errors_preserve_previous_outputs(self):
        cases = [
            ('annotate("GUEST_ENTRY:")', "malformed GUEST_ENTRY"),
            ('annotate("GUEST_ENTRY:bad-name")', "malformed GUEST_ENTRY"),
            ('annotate("GUEST_ENTRY:class")', "malformed GUEST_ENTRY"),
            ('annotate("GUEST_ENTRY:entry"), annotate("GUEST_ENTRY:entry")', "duplicate GUEST_ENTRY"),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.generate(directory, PROTOTYPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            outputs = [directory / "fixture.S", directory / "fixture_trampolines.h"]
            previous = [(output.read_bytes(), output.stat().st_mtime_ns) for output in outputs]
            for annotation, diagnostic in cases:
                with self.subTest(annotation=annotation):
                    source = PROTOTYPE.replace('annotate("CC:stdcall")', 'annotate("CC:stdcall"), ' + annotation)
                    result = self.generate(directory, source)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertIn("Cannot generate trampolines", result.stderr)
                    self.assertEqual(
                        [(output.read_bytes(), output.stat().st_mtime_ns) for output in outputs], previous
                    )

    def test_guest_stack_cursor_uses_named_arguments_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.generate(directory, STACK_VARARGS_PROTOTYPE, ["--guest-arch", "x86"])
            self.assertEqual(result.returncode, 0, result.stderr)
            assembly = (directory / "fixture.S").read_text()
            self.assertIn("lea rdx, [r10+28]", assembly)
            self.assertIn("call", assembly)
            self.assertIn("src=guest-stack-cursor", assembly)
            self.assertNotIn("ret 12", assembly)
            self.assertNotIn("[r10+28]", assembly.replace("lea rdx, [r10+28]", ""))
            mapping = (directory / "fixture_trampolines.h").read_text()
            self.assertIn("thunk_fixture_Probe(void * arg0, const unsigned short * arg1, ...)", mapping)
            self.assertNotIn("arg2", mapping)

            result = self.generate(
                directory, STACK_VARARGS_PROTOTYPE, ["--arch", "x86", "--guest-arch", "x86"]
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            assembly = (directory / "fixture.S").read_text()
            self.assertIn("lea ecx, [eax+12]", assembly)
            self.assertIn("mov [esp+8], ecx", assembly)
            self.assertNotIn("ret 12", assembly)

            source = STACK_VARARGS_PROTOTYPE.replace(
                'annotate("CC:cdecl")', 'fastcall, annotate("CC:cdecl")'
            )
            result = self.generate(directory, source, ["--arch", "x86", "--guest-arch", "x86"])
            self.assertEqual(result.returncode, 0, result.stderr)
            assembly = (directory / "fixture.S").read_text()
            self.assertIn("lea ecx, [eax+12]", assembly)
            self.assertIn("mov [esp+0], ecx", assembly)
            self.assertIn("mov ecx, [eax+4]", assembly)
            self.assertIn("mov edx, [eax+8]", assembly)
            self.assertNotIn("ret 12", assembly)

    def test_guest_stack_cursor_errors_preserve_previous_outputs(self):
        cases = [
            (
                'typedef int (*Callback)(const void *) '
                '__attribute__((annotate("CC:cdecl"), annotate("GUEST_STACK_VARARGS")));',
                ["--guest-arch", "x86"], "callback typedefs",
            ),
            (STACK_VARARGS_PROTOTYPE, [], "requires a 32-bit guest"),
            (
                STACK_VARARGS_PROTOTYPE.replace("CC:cdecl", "CC:stdcall"),
                ["--guest-arch", "x86"], "fixed cdecl",
            ),
            (
                STACK_VARARGS_PROTOTYPE.replace("const void *arguments", "int arguments"),
                ["--guest-arch", "x86"], "final pointer",
            ),
            (
                STACK_VARARGS_PROTOTYPE.replace("const void *arguments", "const void *arguments, ..."),
                ["--guest-arch", "x86"], "fixed cdecl",
            ),
            (
                STACK_VARARGS_PROTOTYPE.replace(
                    'annotate("GUEST_STACK_VARARGS")', 'annotate("GUEST_STACK_VARARGS:bad")'
                ),
                ["--guest-arch", "x86"], "malformed GUEST_STACK_VARARGS",
            ),
            (
                STACK_VARARGS_PROTOTYPE.replace(
                    'annotate("GUEST_STACK_VARARGS")',
                    'annotate("GUEST_STACK_VARARGS"), annotate("GUEST_STACK_VARARGS")',
                ),
                ["--guest-arch", "x86"], "duplicate GUEST_STACK_VARARGS",
            ),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = self.generate(directory, PROTOTYPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            outputs = [directory / "fixture.S", directory / "fixture_trampolines.h"]
            previous = [(output.read_bytes(), output.stat().st_mtime_ns) for output in outputs]
            for source, arguments, diagnostic in cases:
                with self.subTest(diagnostic=diagnostic):
                    result = self.generate(directory, source, arguments)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(diagnostic, result.stderr)
                    self.assertEqual(
                        [(output.read_bytes(), output.stat().st_mtime_ns) for output in outputs], previous
                    )


if __name__ == "__main__":
    unittest.main()
