#!/bin/bash
set -euo pipefail
security_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
test_environment="$security_dir/.research/venv"
python3 -m venv "$test_environment"
"$test_environment/bin/python" -m pip install --disable-pip-version-check --no-cache-dir --requirement "$security_dir/scripts/test-requirements.txt"
"$test_environment/bin/python" -c 'import cryptography, google.protobuf; assert cryptography.__version__ == "46.0.5"; assert google.protobuf.__version__ == "6.33.2"; print("Pinned local cryptographic test environment is ready.")'
