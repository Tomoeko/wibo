"""Check bounded CLR classification and explicit standalone runtime forwarding."""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


PARSER = argparse.ArgumentParser()
PARSER.add_argument("runner", type=Path)
PARSER.add_argument("--machine", choices=["x86", "x86_64"], required=True)
OPTIONS, REMAINING = PARSER.parse_known_args()
RUNNER = OPTIONS.runner.resolve()


def image():
    wide = OPTIONS.machine == "x86_64"
    optional_size = 240 if wide else 224
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    struct.pack_into("<4sHHIIIHH", data, 128, b"PE\0\0", 0x8664 if wide else 0x14C,
                     1, 0, 0, 0, optional_size, 0x22 if wide else 0x102)
    optional = 152
    struct.pack_into("<H", data, optional, 0x20B if wide else 0x10B)
    if wide:
        struct.pack_into("<Q", data, optional + 24, 0x140000000)
    else:
        struct.pack_into("<I", data, optional + 28, 0x400000)
    struct.pack_into("<II", data, optional + 32, 4096, 512)
    struct.pack_into("<II", data, optional + 56, 0x3000, 512)
    struct.pack_into("<H", data, optional + 68, 3)
    struct.pack_into("<I", data, optional + (108 if wide else 92), 16)
    directories = optional + (112 if wide else 96)
    struct.pack_into("<II", data, directories + 5 * 8, 0x2100, 8)
    struct.pack_into("<II", data, directories + 14 * 8, 0x2000, 72)
    section = optional + optional_size
    struct.pack_into("<8s6I2HI", data, section, b".text\0\0\0", 512, 0x2000, 512, 512,
                     0, 0, 0, 0, 0x60000020)
    struct.pack_into("<IHHIIII", data, 512, 72, 2, 5, 0x2050, 32, 1, 0x06000001)
    data[592:596] = b"BSJB"
    struct.pack_into("<II", data, 768, 0, 8)
    return data, directories + 14 * 8


class ManagedRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="wibo-managed-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.work = self.root / "managed work"
        self.work.mkdir()
        self.report = self.root / "report.json"
        self.runtime = self.root / "managed runtime"
        self.runtime.write_text(
            "#!/usr/bin/env python3\nimport json,os,sys\n"
            "with open(os.environ['WIBO_MANAGED_TEST_REPORT'],'w') as output:\n"
            " json.dump({'arguments':sys.argv[1:],'cwd':os.getcwd()},output)\n"
            "sys.exit(37)\n")
        self.runtime.chmod(0o700)
        self.input = self.root / "input.exe"
        self.data, self.clr_directory = image()
        self.input.write_bytes(self.data)
        self.environment = dict(os.environ, WIBO_MANAGED_RUNTIME=str(self.runtime),
                                WIBO_MANAGED_TEST_REPORT=str(self.report))

    def invoke(self, *arguments, environment=None):
        return subprocess.run([str(RUNNER), *map(str, arguments)],
                              env=self.environment if environment is None else environment,
                              capture_output=True, text=True, timeout=5, check=False)

    def test_forwards_exact_arguments_and_working_directory(self):
        arguments = ["two words", "", 'quote"value', "tail\\", "\u03bb"]
        result = self.invoke("-C", self.work, self.input, *arguments)
        self.assertEqual(result.returncode, 37, result.stderr)
        report = json.loads(self.report.read_text())
        self.assertEqual(report, {"arguments": [str(self.input), *arguments], "cwd": str(self.work)})

    def test_forwards_the_parsed_explicit_command_line(self):
        arguments = ["two words", "", 'quote"value', "tail\\"]
        command = subprocess.list2cmdline(["custom-argv-zero", *arguments])
        result = self.invoke("--cmdline", command, self.input)
        self.assertEqual(result.returncode, 37, result.stderr)
        self.assertEqual(json.loads(self.report.read_text())["arguments"], [str(self.input), *arguments])

    def test_requires_an_explicit_runtime(self):
        environment = dict(self.environment)
        environment.pop("WIBO_MANAGED_RUNTIME")
        result = self.invoke(self.input, environment=environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CLR runner", result.stderr)
        self.assertFalse(self.report.exists())

    def test_reports_missing_runtime_executable(self):
        environment = dict(self.environment, WIBO_MANAGED_RUNTIME=str(self.root / "missing"))
        result = self.invoke(self.input, environment=environment)
        self.assertEqual(result.returncode, 127)
        self.assertIn("Failed to start configured CLR runner", result.stderr)

    def test_rejects_malformed_clr_ranges_and_metadata_before_delegation(self):
        mutations = [(self.clr_directory, 0), (self.clr_directory, 0xFFFFFFFF),
                     (self.clr_directory + 4, 71), (512, 71), (512, 73),
                     (520, 0xFFFFFFFF), (524, 15), (524, 0xFFFFFFFF), (592, 0)]
        for offset, value in mutations:
            with self.subTest(offset=offset, value=value):
                data = bytearray(self.data)
                struct.pack_into("<I", data, offset, value)
                self.input.write_bytes(data)
                result = self.invoke(self.input)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.report.exists())

    def test_does_not_delegate_native_images_without_an_entry_point(self):
        struct.pack_into("<II", self.data, self.clr_directory, 0, 0)
        self.input.write_bytes(self.data)
        result = self.invoke(self.input)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no entry point", result.stderr)
        self.assertFalse(self.report.exists())


if __name__ == "__main__":
    unittest.main(argv=[__file__, *REMAINING])
