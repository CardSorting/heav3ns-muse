"""Original SBI register fixtures for native OpulenZ tests. SPDX-License-Identifier: GPL-2.0-or-later"""
import argparse
from pathlib import Path


PATCHES = {
    "native-opl-carrier": ("Native carrier", bytes([0x21, 0x21, 0x3f, 0x00, 0xf0, 0xf0, 0x0f, 0x0f, 0, 0, 0])),
    "native-opl-octave": ("Native octave", bytes([0x21, 0x22, 0x3f, 0x00, 0xf0, 0xf0, 0x0f, 0x0f, 0, 0, 0])),
    "native-opl-bitfields": ("Native register test", bytes([0xd5, 0xa2, 0xa9, 0x47, 0xc3, 0x8b, 0xa7, 0x6e, 0xff, 0xfe, 0x0d])),
}


def patch_bytes(name, registers):
    label = name.encode("ascii")
    assert len(label) <= 32 and len(registers) == 11
    return b"SBI\x1a" + label.ljust(32, b"\0") + registers + bytes(5)


def generate(output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    paths = []
    for identifier, (name, registers) in PATCHES.items():
        path = output / (identifier + ".sbi")
        path.write_bytes(patch_bytes(name, registers))
        paths.append(path)
    return paths


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    for path in generate(arguments.output):
        print(path.name)
