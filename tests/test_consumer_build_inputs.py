"""Fast checks for native build inputs and public header boundaries."""
import sys
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import build_consumer as build
import build_sdk


class NativeInputs(unittest.TestCase):
    def test_missing_toolchain_fails_before_packaging(self):
        with patch.object(build, "get_public_ps5_toolchain", return_value=(None, None)):
            with self.assertRaisesRegex(SystemExit, "Native consumer inputs missing"):
                build.native_inputs()

    def test_compiler_archive_requires_current_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "libpsbc.ps5.a"
            archive.write_bytes(b"test archive")
            identity = dict(schema=1, target="ps5", source_commit="revision",
                archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                source_patch_sha256=build_sdk.source_patch_digest())
            stamp = archive.with_suffix(".json")
            stamp.write_text(json.dumps(identity))
            build_sdk.validate_compiler_archive(archive, "revision")
            identity["source_patch_sha256"] = "stale"
            stamp.write_text(json.dumps(identity))
            with self.assertRaisesRegex(RuntimeError, "stale"):
                build_sdk.validate_compiler_archive(archive, "revision")

    def test_header_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dep = root / "main.d"
            sdk = root / "sdk"
            for header in (sdk / "target/include/stdint.h",
                           build.DIST_SDK / "include/vulkan/vulkan.h"):
                dep.write_text(f"main.o: {header}\n")
                with patch.object(build.subprocess, "check_output", return_value=""):
                    build.check_isolation(dep, root / "main.o", sdk)
            for header in (sdk / "target/include-private/secret.h",
                           build.DIST_SDK / "include-private/secret.h",
                           root / "ps5-native-app-boilerplate/private.h",
                           build.ROOT / "native/private.h"):
                dep.write_text(f"main.o: {header}\n")
                with self.assertRaisesRegex(AssertionError, "Isolation violation"):
                    build.check_isolation(dep, root / "main.o", sdk)


if __name__ == "__main__":
    unittest.main()
