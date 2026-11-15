"""Execute real display code through allocation, storage and game transitions."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DisplayWorkspaceTests(unittest.TestCase):
    def test_candidate_and_unchanged_default_display(self):
        cjson = ROOT / "managed_components/espressif__cjson/cJSON"
        if not cjson.exists():
            self.skipTest("Resolve pinned cJSON with the firmware build first")
        with tempfile.TemporaryDirectory() as tmp:
            temp = Path(tmp)
            assembly = [".section __TEXT,__const" if sys.platform == "darwin" else ".section .rodata"]
            for symbol, path in [("neko", ROOT / "assets/neko/neko.ecl"),
                                 ("study", ROOT / "assets/study/study.ecl")]:
                assembly += [f".globl _binary_{symbol}_ecl_start", f".globl _binary_{symbol}_ecl_end",
                             f"_binary_{symbol}_ecl_start:", f'.incbin "{path}"', f"_binary_{symbol}_ecl_end:"]
            if sys.platform != "darwin":
                assembly.append('.section .note.GNU-stack,"",@progbits')
            (temp / "assets.s").write_text("\n".join(assembly) + "\n")
            for lights in (0, 1):
                binary = temp / f"display-{lights}"
                subprocess.run([
                    "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-Wno-deprecated-declarations",  # Pinned vendor cJSON uses sprintf on macOS.
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", f"-DENTRANCE_LIGHTS={lights}",
                    "-I", str(ROOT / "tests/native/display_include"),
                    "-I", str(ROOT / "tests/native/include"),
                    "-I", str(ROOT / "components/entrance/include"), "-I", str(cjson),
                    str(ROOT / "tests/native/display_workspace_test.c"),
                    *[str(ROOT / f"components/entrance/{name}.c") for name in ("builtin_loop", "media", "core", "captions", "display_config")],
                    str(cjson / "cJSON.c"), str(temp / "assets.s"), "-lm", "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True, timeout=60)
                for saved_mode in ("replay", "clip", "blank"):
                    subprocess.run([str(binary), saved_mode], check=True, timeout=60)
