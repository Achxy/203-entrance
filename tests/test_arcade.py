"""Run the production game simulation with memory/undefined-behavior sanitizers."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class ArcadeTests(unittest.TestCase):
    def test_native_games_sessions_and_adversarial_input(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / 'arcade-test'
            subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                '-I',str(ROOT/'components/entrance/include'),
                str(ROOT/'tests/native/arcade_test.c'),str(ROOT/'components/entrance/arcade_core.c'),
                '-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=60)

    def test_public_controller_matches_embedded_assets(self):
        for source, public in [('play.html', 'controller.html'), ('play.js', 'play.js'), ('play.css', 'play.css'), ('relay.js', 'relay.js')]:
            self.assertEqual((ROOT / 'player' / source).read_bytes(), (ROOT / 'sites/arcade/public' / public).read_bytes())
