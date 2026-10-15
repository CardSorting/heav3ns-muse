"""Generate the small original PCM SoundFont used only by native integration tests.

Two presets share a sine sample; the second instrument transposes it by an
octave. A second fixture layers six sample zones in the first instrument.
Production LMMS does not import this generator or use a song template.
The native FluidSynth loader remains responsible for accepting this fixture.
"""
import math
from pathlib import Path
import struct


def chunk(identifier, data):
    return identifier + struct.pack("<I", len(data)) + data + (b"\0" if len(data) % 2 else b"")


def soundfont(layered=False):
    rate, frames = 44100, 44100
    pcm = b"".join(struct.pack("<h", round(12000 * math.sin(2 * math.pi * 440 * i / rate))) for i in range(frames)) + bytes(46 * 2)
    info = chunk(b"LIST", b"INFO" + chunk(b"ifil", struct.pack("<HH", 2, 1))
                 + chunk(b"isng", b"EMU8000\0") + chunk(b"INAM", b"LMMS native calibration\0"))
    samples = chunk(b"LIST", b"sdta" + chunk(b"smpl", pcm))
    presets = b"".join(struct.pack("<20sHHHIII", name, program, bank, bag, 0, 0, 0) for name, program, bank, bag in (
        (b"Native base", 0, 0, 0), (b"Native octave", 7, 1024, 1), (b"EOP", 0, 0, 2)))
    layers = 6 if layered else 1
    instruments = b"".join(struct.pack("<20sH", name, bag) for name, bag in (
        (b"Base oscillator", 0), (b"Octave oscillator", layers), (b"EOI", layers + 1)))
    headers = struct.pack("<20sIIIIIBbHH", b"Original sine", 0, frames, 128, frames - 128, rate, 69, 0, 0, 1)
    headers += struct.pack("<20sIIIIIBbHH", b"EOS", 0, 0, 0, 0, 0, 0, 0, 0, 0)
    tables = (
        (b"phdr", presets),
        (b"pbag", b"".join(struct.pack("<HH", i, 0) for i in (0, 1, 2))),
        (b"pmod", bytes(10)),
        (b"pgen", struct.pack("<HHHHHH", 41, 0, 41, 1, 0, 0)),
        (b"inst", instruments),
        (b"ibag", b"".join(struct.pack("<HH", i, 0) for i in [2 * zone for zone in range(layers + 1)] + [2 * layers + 3])),
        (b"imod", bytes(10)),
        (b"igen", b"".join(struct.pack("<HH", operation, value) for operation, value in
                           [(54, 1), (53, 0)] * layers + [(51, 12), (54, 1), (53, 0), (0, 0)])),
        (b"shdr", headers),
    )
    definitions = chunk(b"LIST", b"pdta" + b"".join(chunk(identifier, data) for identifier, data in tables))
    return chunk(b"RIFF", b"sfbk" + info + samples + definitions)


if __name__ == "__main__":
    destination = Path(__file__).with_name("native-calibration.sf2")
    data = soundfont()
    destination.write_bytes(data)
    layered = soundfont(layered=True)
    destination.with_name("native-layered.sf2").write_bytes(layered)
    print(f"Generated {len(data)} calibration bytes and {len(layered)} layered bytes; no native loader or DSP verification performed")
