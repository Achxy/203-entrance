import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
class RelayTests(unittest.TestCase):
    def test_owned_ingress(self):
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        if not cjson.exists():
            self.skipTest('Run the pinned firmware build to resolve cJSON first')
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'relay-ingress-test'
            vendor_object = pathlib.Path(tmp) / 'cjson.o'
            flags = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                     '-fsanitize=address,undefined', '-DCJSON_NESTING_LIMIT=8',
                     '-I', str(ROOT / 'components/entrance/include'), '-I', str(cjson)]
            # Current macOS SDK deprecates sprintf used by pinned upstream cJSON.
            # Suppress it only for that vendor object, not the ingress/test code.
            subprocess.run(flags + ['-Wno-deprecated-declarations', '-c', str(cjson / 'cJSON.c'),
                                    '-o', str(vendor_object)], check=True)
            subprocess.run(flags + [
                            str(ROOT / 'components/entrance/relay_ingress.c'),
                            str(ROOT / 'components/entrance/core.c'),
                            str(ROOT / 'components/entrance/protocol.c'), str(vendor_object),
                            str(ROOT / 'tests/native/relay_ingress_test.c'),
                            '-lm', '-o', str(binary)], check=True)
            for scenario in ('bounds', 'mixed_fifo', 'queue_full', 'resources',
                             'maximum_burst', 'malformed_reconnect', 'concurrency'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=30)

    def test_address_failover_transport(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'relay-transport-test'
            subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined',
                            '-Dgetaddrinfo=test_getaddrinfo', '-Dfreeaddrinfo=test_freeaddrinfo',
                            '-I', str(ROOT / 'tests/native/transport_include'),
                            '-I', str(ROOT / 'tests/native/include'),
                            '-I', str(ROOT / 'components/entrance/include'),
                            str(ROOT / 'components/entrance/relay_transport.c'),
                            str(ROOT / 'tests/native/relay_transport_test.c'),
                            '-o', str(binary)], check=True)
            for scenario in ('failover', 'redirects', 'resources', 'dns'):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=30)

    def test_fragmentation_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / 'relay-test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'components/entrance/include'),
                            str(ROOT / 'tests/native/relay_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
