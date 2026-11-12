"""Wii BNS (banner sound, DSP-ADPCM) -> 16-bit PCM WAV.

Usage:  python -I tools/banner/bns.py in.bns out.wav
decode_bns(data) -> (sample_rate, [channel pcm lists], loop_start or None)
"""
import struct
import sys
import wave


def _clamp(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def decode_bns(d):
    if d[:4] != b"BNS ":
        raise ValueError("not BNS")
    info_off, info_size, data_off = struct.unpack(">III", d[0x10:0x1C])
    info = d[info_off:info_off + info_size]
    codec, loop, nch, rate, loop_start, nsamples = struct.unpack(">BBBxHxxII", info[8:24])
    ch_tab_off = struct.unpack(">I", info[0x18:0x1C])[0]
    data = d[data_off + 8:]
    chans = []
    for c in range(nch):
        ch_off = struct.unpack(">I", info[8 + ch_tab_off + c * 4:12 + ch_tab_off + c * 4])[0]
        sdata_off, adpcm_off = struct.unpack(">II", info[8 + ch_off:16 + ch_off])
        coefs = struct.unpack(">16h", info[8 + adpcm_off:8 + adpcm_off + 32])
        hist1, hist2 = struct.unpack(">hh", info[8 + adpcm_off + 36:8 + adpcm_off + 40])
        src = data[sdata_off:]
        out = []
        p = 0
        while len(out) < nsamples:
            ps = src[p]
            p += 1
            scale = 1 << (ps & 15)
            c1, c2 = coefs[(ps >> 4) * 2], coefs[(ps >> 4) * 2 + 1]
            for i in range(14):
                if len(out) >= nsamples:
                    break
                b = src[p + i // 2]
                n = (b >> 4) if i % 2 == 0 else (b & 15)
                if n >= 8:
                    n -= 16
                s = _clamp((n * scale * 2048 + 1024 + c1 * hist1 + c2 * hist2) >> 11)
                out.append(s)
                hist2, hist1 = hist1, s
            p += 7
        chans.append(out)
    return rate, chans, (loop_start if loop else None)


if __name__ == "__main__":
    rate, chans, _ = decode_bns(open(sys.argv[1], "rb").read())
    with wave.open(sys.argv[2], "wb") as w:
        w.setnchannels(len(chans))
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(len(chans[0])):
            for c in chans:
                frames += struct.pack("<h", c[i])
        w.writeframes(bytes(frames))
    print(f"{rate} Hz, {len(chans)} ch, {len(chans[0])} samples ({len(chans[0]) / rate:.2f} s) -> {sys.argv[2]}")
