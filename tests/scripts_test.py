"""Host-side checks for snapshot parsing and helper-script interfaces.

Run with: python3 -m unittest discover -s tests -p '*_test.py'
The compiler is mocked; these tests do not execute RISC-V code.
"""

import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
spec = importlib.util.spec_from_file_location(
    "snapshot_regs", SCRIPTS / "snapshot_regs.py")
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)


class ScriptTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="runshot test ")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.snapshot = self.root / "snapshot.img"

    def write_snapshot(self, mask=6, csr_mask=0):
        values = [0x1000 + index for index in range(64)]
        csrs = [1, 2, 3]
        data = struct.pack("<IIQQB", 0x52454753, 25, 0x85000100,
                           mask, csr_mask)
        data += b"".join(struct.pack("<Q", value)
                         for index, value in enumerate(values)
                         if mask & (1 << index))
        data += b"".join(struct.pack("<Q", value)
                         for index, value in enumerate(csrs)
                         if csr_mask & (1 << index))
        self.snapshot.write_bytes(data)
        return values, csrs

    def test_sparse_registers(self):
        self.write_snapshot()
        pc, mask, csr_mask, regs, csrs = parser.read_registers(self.snapshot)
        self.assertEqual((pc, mask, csr_mask), (0x85000100, 6, 0))
        self.assertEqual(regs, [0, 0x1001, 0x1002] + [0] * 61)
        self.assertEqual(csrs, [0, 0, 0])

    def test_full_register_image(self):
        values, csrs = self.write_snapshot((1 << 64) - 1, 7)
        self.assertEqual(parser.read_registers(self.snapshot)[3:],
                         (values, csrs))

    def test_truncated_payloads(self):
        self.write_snapshot((1 << 64) - 1, 7)
        data = self.snapshot.read_bytes()
        for size in (0, 24, 25, 32, len(data) - 1):
            with self.subTest(size=size):
                self.snapshot.write_bytes(data[:size])
                with self.assertRaises(ValueError):
                    parser.read_registers(self.snapshot)

    def test_required_registers(self):
        for mask, missing in ((2, "sp"), (4, "ra")):
            with self.subTest(mask=mask):
                self.write_snapshot(mask)
                with self.assertRaisesRegex(ValueError, missing):
                    parser.read_registers(self.snapshot)

    def test_bad_header(self):
        self.snapshot.write_bytes(struct.pack("<IIQQB", 1, 25, 0, 6, 0))
        with self.assertRaisesRegex(ValueError, "header"):
            parser.read_registers(self.snapshot)
        self.snapshot.write_bytes(struct.pack("<IIQQB", 0x52454753, 24,
                                              0, 6, 0))
        with self.assertRaisesRegex(ValueError, "header"):
            parser.read_registers(self.snapshot)

    def test_detect_format(self):
        for data, expected in ((b"\x7fELF", "elf"),
                               (struct.pack("<I", 0x52454753), "snapshot"),
                               (b"", "unknown"), (b"text", "unknown")):
            self.snapshot.write_bytes(data)
            self.assertEqual(parser.detect_format(self.snapshot), expected)

    def tool(self, name, body):
        path = self.root / name
        path.write_text("#!/usr/bin/env bash\nset -eu\n" + body)
        path.chmod(0o755)
        return str(path)

    def run_script(self, script, *args, **env):
        return subprocess.run(
            ["bash", str(SCRIPTS / script), *map(str, args)], cwd=self.root,
            env={**os.environ, **env}, capture_output=True, text=True)

    def compiler(self):
        return self.tool("compiler", 'printf "%s\\n" "$@" > compiler.args\n'
                         'while [[ $# -gt 0 ]]; do\n'
                         '  if [[ "$1" == -o ]]; then touch "$2"; break; fi\n'
                         '  shift\ndone\n')

    def test_build_from_other_directory(self):
        source = self.root / "input program.c"
        source.write_text("int main(void) { return 0; }\n")
        result = self.run_script("build.sh", source, "-o", "test program",
                                 CC=self.compiler())
        self.assertEqual(result.returncode, 0, result.stderr)
        args = (self.root / "compiler.args").read_text().splitlines()
        self.assertIn(str(SCRIPTS / "wrap_stack.c"), args)
        self.assertIn(str(source), args)
        self.assertIn("-Wl,--wrap=__libc_start_main", args)
        self.assertTrue((self.root / "test program.riscv").exists())

    def test_elf_entry_and_custom_linker(self):
        self.snapshot.write_bytes(b"\x7fELF")
        readelf = self.tool("readelf", '''if [[ "$1" == -l ]]; then
  echo 'Entry point 0x85000100'
else
  echo '1: 0000000084999990 8 OBJECT GLOBAL DEFAULT 1 tohost'
  echo '2: 0000000084999998 8 OBJECT GLOBAL DEFAULT 1 fromhost'
fi
''')
        linker = self.root / "entry.ld"
        linker.write_text("/* custom linker */\n")
        result = self.run_script("make_entry.sh", self.snapshot,
                                 READELF=readelf, GCC=self.compiler())
        self.assertEqual(result.returncode, 0, result.stderr)
        assembly = (self.root / "entry.S").read_text()
        self.assertIn("li   t0, 0x85000100", assembly)
        self.assertIn(".set   tohost, 0x0000000084999990", assembly)
        self.assertEqual(linker.read_text(), "/* custom linker */\n")

    def test_invalid_snapshot_stops_before_compiler(self):
        self.snapshot.write_bytes(struct.pack("<I", 0x52454753))
        result = self.run_script("make_entry.sh", self.snapshot,
                                 READELF=shutil.which("true"),
                                 GCC=self.compiler())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("snapshot too small", result.stderr)
        self.assertFalse((self.root / "compiler.args").exists())

    def test_snapshot_entry(self):
        version = subprocess.check_output(
            ["bash", "-c", "echo ${BASH_VERSINFO[0]}"], text=True)
        if int(version) < 4:
            self.skipTest("snapshot shell path requires Linux Bash 4+")
        self.write_snapshot()
        result = self.run_script("make_entry.sh", self.snapshot,
                                 READELF=shutil.which("true"),
                                 GCC=self.compiler())
        self.assertEqual(result.returncode, 0, result.stderr)
        assembly = (self.root / "entry.S").read_text()
        self.assertEqual(assembly.count("    .dword 0x"), 68)
        self.assertIn("ld ra,  16(ra)\n    jr ra", assembly)
        self.assertTrue((self.root / "entry.ld").exists())


if __name__ == "__main__":
    unittest.main()
