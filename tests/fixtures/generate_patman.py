"""Original GUS-format PCM calibration assets for native Patman tests only.

These bytes exercise LMMS's own loader and Sample playback. No production code
imports this generator, and generation does not prove native loader acceptance.
"""
import math
from pathlib import Path
import struct


def patch(encoding="s16"):
    assert encoding in ("s8", "u8", "s16", "u16")
    unsigned, sixteen = encoding.startswith("u"), encoding.endswith("16")
    rate, frames = 22050, 22050
    width = 2 if sixteen else 1
    amplitude = 16000 if sixteen else 100
    waves = []
    for frequency, root in ((440, 440), (660, 880)):
        header = bytearray(96)
        header[:7] = (f"GUS{frequency}").encode("ascii").ljust(7, b"\0")
        struct.pack_into("<IIIHIII", header, 8, frames * width, 2205 * width,
                         19845 * width, rate, 0, 4294967295, root * 1000)
        header[36] = 7
        header[55] = int(sixteen) | (2 if unsigned else 0) | 4
        samples = [round(amplitude * math.sin(2 * math.pi * frequency * frame / rate)) for frame in range(frames)]
        if unsigned:
            samples = [value + (32768 if sixteen else 128) for value in samples]
        code = ("H" if unsigned else "h") if sixteen else ("B" if unsigned else "b")
        pcm = b"".join(struct.pack("<" + code, value) for value in samples)
        waves.append(bytes(header) + pcm)
    payload = b"".join(waves)
    header = bytearray(239)
    header[:22] = b"GF1PATCH110\0ID#000002\0"
    header[22:82] = b"Original LMMS native calibration".ljust(60, b"\0")
    header[82:85] = bytes((1, 14, 1))
    struct.pack_into("<HHI", header, 85, len(waves), 127, len(payload) + 110)
    header[131:147] = b"Native fixture".ljust(16, b"\0")
    struct.pack_into("<I", header, 147, len(payload) + 47)
    header[151] = 1
    struct.pack_into("<I", header, 194, len(payload))
    header[198] = len(waves)
    return bytes(header) + payload


if __name__ == "__main__":
    for encoding in ("s8", "u8", "s16", "u16"):
        data = patch(encoding)
        destination = Path(__file__).with_name(f"native-gus-{encoding}.pat")
        destination.write_bytes(data)
        print(f"Generated {destination.name}: {len(data)} original fixture bytes; native loader/DSP not run")
