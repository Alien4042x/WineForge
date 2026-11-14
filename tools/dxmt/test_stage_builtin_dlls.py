import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("stage_dxmt", Path(__file__).with_name("stage_builtin_dlls.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def pe(machine, magic):
    data = bytearray(128)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<H", data, 68, machine)
    struct.pack_into("<H", data, 86, 0x2000)
    struct.pack_into("<H", data, 88, magic)
    return bytes(data)


class BuiltinStagingTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name).resolve()
        for architecture, (machine, magic) in module.ARCHITECTURES.items():
            source = self.source(architecture)
            source.parent.mkdir(parents=True)
            source.write_bytes(pe(machine, magic))
            self.target(architecture).parent.mkdir(parents=True)

    def source(self, architecture):
        return self.root / "lib/dxmt" / architecture / "winemetal.dll"

    def target(self, architecture):
        return self.root / "lib/wine" / architecture / "winemetal.dll"

    def builder(self):
        configured = os.environ.get("WINEFORGE_WORKSPACE_BUILDER")
        path = Path(configured) if configured else Path(__file__).resolve().parents[2] / "build-wine-11.12-d3dmetal.sh"
        if not path.is_file():
            self.skipTest("Set WINEFORGE_WORKSPACE_BUILDER to run the workspace integration checks")
        return path

    def runtime_fixture(self):
        dxmt = self.root / "lib/dxmt"
        for relative in ["x86_64-unix/winemetal.so", "x86_64-windows/dxgi.dll",
                         "x86_64-windows/d3d10core.dll", "x86_64-windows/d3d11.dll",
                         "x86_64-windows/nvapi64.dll", "x86_64-windows/nvngx.dll",
                         "i386-windows/dxgi.dll", "i386-windows/d3d10core.dll", "i386-windows/d3d11.dll"]:
            path = dxmt / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        return dxmt

    def test_stages_both_and_is_idempotent(self):
        self.assertEqual([row["state"] for row in module.stage(self.root)], ["staged", "staged"])
        for architecture in module.ARCHITECTURES:
            self.assertEqual(self.source(architecture).read_bytes(), self.target(architecture).read_bytes())
        with mock.patch.object(module, "require_unmapped", side_effect=AssertionError("unchanged DLL")):
            self.assertEqual([row["state"] for row in module.stage(self.root, check=True)],
                             ["verified", "verified"])

    def test_check_does_not_copy_missing_files(self):
        with self.assertRaisesRegex(ValueError, "missing or outdated"):
            module.stage(self.root, check=True)
        self.assertFalse(self.target("i386-windows").exists())

    def test_cli_source_validation_does_not_stage_and_rejects_invalid_source(self):
        command = [sys.executable, str(Path(module.__file__)), str(self.root / "lib/dxmt"), "--validate-source"]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.target("i386-windows").exists())
        self.source("i386-windows").write_bytes(b"")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Not a PE DLL", result.stderr)

    def test_builder_rejects_invalid_source_before_copying_runtime(self):
        builder = self.builder()
        dxmt = self.runtime_fixture()
        self.source("i386-windows").write_bytes(b"")
        install = self.root / "install"
        install.mkdir()
        environment = dict(os.environ, INSTALL=str(install), DXMT_RUNTIME_DIR=str(dxmt))
        result = subprocess.run(["/bin/bash", str(builder), "dxmt-runtime-sync"],
                                env=environment, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Not a PE DLL", result.stderr)
        self.assertEqual(list(install.iterdir()), [])

    def test_builder_invokes_helper_from_selected_source(self):
        builder = self.builder()
        dxmt = self.runtime_fixture()
        source = self.root / "selected-source"
        helper = source / "tools/dxmt/stage_builtin_dlls.py"
        helper.parent.mkdir(parents=True)
        helper.write_text("import sys\nprint('selected-source-helper', file=sys.stderr)\nsys.exit(1)\n")
        install = self.root / "install"
        install.mkdir()
        environment = dict(os.environ, SOURCE=str(source), INSTALL=str(install), DXMT_RUNTIME_DIR=str(dxmt))
        result = subprocess.run(["/bin/bash", str(builder), "dxmt-runtime-sync"],
                                env=environment, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("selected-source-helper", result.stderr)
        self.assertEqual(list(install.iterdir()), [])

    def test_invalid_or_missing_second_source_never_stages_first(self):
        source = self.source("x86_64-windows")
        cases = [b"", b"MZ", pe(0x014C, 0x010B), None]
        for data in cases:
            with self.subTest(data=data):
                if data is None:
                    source.unlink()
                else:
                    source.write_bytes(data)
                with self.assertRaises((ValueError, OSError)):
                    module.stage(self.root)
                self.assertFalse(self.target("i386-windows").exists())

    def test_source_read_error_never_stages_first(self):
        original = Path.read_bytes
        def read(path):
            if path == self.source("x86_64-windows"):
                raise PermissionError("fixture read denied")
            return original(path)
        with mock.patch.object(Path, "read_bytes", read), self.assertRaises(PermissionError):
            module.stage(self.root)
        self.assertFalse(self.target("i386-windows").exists())

    def test_symlink_target_is_rejected_without_following_it(self):
        outside = self.root / "outside"
        outside.write_bytes(b"preserve")
        self.target("x86_64-windows").symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "regular builtin"):
            module.stage(self.root)
        self.assertEqual(outside.read_bytes(), b"preserve")
        self.assertFalse(self.target("i386-windows").exists())

    def test_mapped_target_or_mapping_read_failure_blocks_all_changes(self):
        module.stage(self.root)
        self.source("x86_64-windows").write_bytes(pe(0x8664, 0x020B) + b"updated")
        old = self.target("x86_64-windows").read_bytes()
        with mock.patch.object(module, "require_unmapped", side_effect=ValueError("blocked")):
            with self.assertRaisesRegex(ValueError, "blocked"):
                module.stage(self.root)
        self.assertEqual(self.target("x86_64-windows").read_bytes(), old)

    def test_unmapped_runtime_update_replaces_the_old_builtin(self):
        module.stage(self.root)
        source = self.source("i386-windows")
        source.write_bytes(source.read_bytes() + b"updated")
        with mock.patch.object(module, "require_unmapped") as guard:
            result = module.stage(self.root)
        guard.assert_called_once_with(self.target("i386-windows"))
        self.assertEqual([row["state"] for row in result], ["staged", "verified"])
        self.assertEqual(source.read_bytes(), self.target("i386-windows").read_bytes())

    def test_mapping_status_distinguishes_absence_and_error(self):
        target = self.target("i386-windows")
        for code, stdout, stderr, allowed in [(1, "", "", True), (0, "12\n", "", False),
                                              (1, "", "denied", False), (2, "", "", False)]:
            with self.subTest(code=code, stdout=stdout, stderr=stderr):
                result = module.subprocess.CompletedProcess([], code, stdout, stderr)
                with mock.patch.object(module.subprocess, "run", return_value=result):
                    if allowed:
                        module.require_unmapped(target)
                    else:
                        with self.assertRaises(ValueError):
                            module.require_unmapped(target)


if __name__ == "__main__":
    unittest.main()
