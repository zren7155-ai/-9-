import pathlib
import struct
import wave


ROOT = pathlib.Path(__file__).resolve().parents[1]
VOICE_DIR = ROOT / "assets" / "voice"
ITEMS = [
    ("analyzing", "analyzing_raw.wav"),
    ("fall_detected", "fall_detected_raw.wav"),
    ("fatigue", "fatigue_raw.wav"),
]


def resample(samples, src_rate, dst_rate=16000):
    if not samples:
        return []
    count = max(1, int(len(samples) * dst_rate / src_rate))
    ratio = src_rate / dst_rate
    out = []
    for i in range(count):
        pos = i * ratio
        idx = int(pos)
        frac = pos - idx
        if idx >= len(samples) - 1:
            value = samples[-1]
        else:
            value = samples[idx] * (1.0 - frac) + samples[idx + 1] * frac
        out.append(max(-32768, min(32767, int(value))))
    return out


def load_as_stereo_16k(path):
    with wave.open(str(path), "rb") as wav:
        channels = wav.getnchannels()
        rate = wav.getframerate()
        width = wav.getsampwidth()
        raw = wav.readframes(wav.getnframes())
    if width != 2:
        raise RuntimeError(f"unsupported WAV sample width: {path} width={width}")

    values = list(struct.unpack("<" + "h" * (len(raw) // 2), raw))
    if channels > 1:
        mono = [sum(values[i:i + channels]) // channels for i in range(0, len(values), channels)]
    else:
        mono = values

    mono = resample(mono, rate, 16000)
    stereo = []
    for sample in mono:
        stereo.extend([sample, sample])
    return stereo


def main():
    converted = [(name, load_as_stereo_16k(VOICE_DIR / filename)) for name, filename in ITEMS]

    header = ROOT / "main" / "voice_assets.h"
    source = ROOT / "main" / "voice_assets.c"

    with header.open("w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n")
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        for name, _ in converted:
            f.write(f"extern const int16_t g_voice_{name}_pcm[];\n")
            f.write(f"extern const size_t g_voice_{name}_pcm_len;\n")
        f.write("\n#ifdef __cplusplus\n}\n#endif\n")

    with source.open("w", encoding="utf-8", newline="\n") as f:
        f.write('#include "voice_assets.h"\n\n')
        for name, data in converted:
            f.write(f"const int16_t g_voice_{name}_pcm[] = {{\n")
            for i in range(0, len(data), 16):
                f.write("    " + ", ".join(str(v) for v in data[i:i + 16]) + ",\n")
            f.write("};\n")
            f.write(
                f"const size_t g_voice_{name}_pcm_len = "
                f"sizeof(g_voice_{name}_pcm) / sizeof(g_voice_{name}_pcm[0]);\n\n"
            )

    print("generated", source, header, [(name, len(data)) for name, data in converted])


if __name__ == "__main__":
    main()
