#!/usr/bin/env python3
"""Feed pinned IDF descriptors to Apple's standard protoc-gen-swift plugin.

This avoids relying on a globally installed protoc binary. The plugin is built
from the exact SwiftProtobuf revision in Package.resolved. No wire schema changes.
"""
import os
from pathlib import Path
import subprocess
import sys

from google.protobuf.compiler import plugin_pb2

PACKAGE = Path(__file__).resolve().parents[1]
IDF = PACKAGE.parents[1] / ".tools/esp-idf"
os.environ["IDF_PATH"] = str(IDF)
sys.path.insert(0, str(IDF / "examples/protocols/esp_local_ctrl/scripts"))
import proto  # noqa: E402

request = plugin_pb2.CodeGeneratorRequest(parameter="Visibility=Package")
def complete_json_names(message, descriptor):
    for field in message.field:
        field.json_name = descriptor.fields_by_name[field.name].json_name
    for nested in message.nested_type:
        complete_json_names(nested, descriptor.nested_types_by_name[nested.name])

for name in ("constants", "sec0", "sec1", "sec2", "session"):
    descriptor = getattr(proto, name + "_pb2").DESCRIPTOR
    request.file_to_generate.append(name + ".proto")
    file = request.proto_file.add()
    descriptor.CopyToProto(file)
    # IDF's old generated descriptors omit names that protoc normally computes.
    for message in file.message_type:
        complete_json_names(message, descriptor.message_types_by_name[message.name])
generator = PACKAGE / ".research/protoc-build/debug/protoc-gen-swift"
result = subprocess.run([str(generator)], input=request.SerializeToString(), capture_output=True)
if result.returncode:
    raise SystemExit(result.stderr.decode()[:3000])
response = plugin_pb2.CodeGeneratorResponse.FromString(result.stdout)
if response.error:
    raise SystemExit(response.error)
for file in response.file:
    name = Path(file.name)
    if name.name != file.name or not name.name.endswith(".pb.swift"):
        raise SystemExit("Unexpected generated filename")
    (PACKAGE / "Sources/LightsSecurity/Proto" / name).write_text(file.content)
print(f"Regenerated {len(response.file)} Swift protobuf sources from unmodified IDF descriptors.")
