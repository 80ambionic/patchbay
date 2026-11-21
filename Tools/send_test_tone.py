#!/usr/bin/env python3
"""Send a test tone to PatchBay's Network page over UDP (reference implementation of the wire format).

  python3 send_test_tone.py 192.168.1.20 --port 9000 --channels 2 --freq 440

Real-time paced. Use --drift-ppm to run this sender's clock fast/slow and watch the Clock correction
on the Network page follow it; --loss 0.02 drops 2% of packets to watch concealment.
Wire format (all header fields big-endian, payload little-endian): see Source/Net/Packet.h.
"""
import argparse, math, random, socket, struct, time

HEADER = struct.Struct(">IBBHIHBB")        # magic, version, type, seq, timestamp, payloadBytes, channels, format
MAGIC, VERSION, TYPE_AUDIO = 0x50424E54, 1, 1
FMT_FLOAT32, FMT_INT16 = 0, 1

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host"); ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--channels", type=int, default=2); ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--frames", type=int, default=128, help="frames per packet (keep header+payload under ~1400 bytes)")
    ap.add_argument("--freq", type=float, default=440.0, help="tone frequency in Hz (each channel is offset a little in pitch)")
    ap.add_argument("--format", choices=["float32", "int16"], default="float32")
    ap.add_argument("--seconds", type=float, default=0, help="0 = run until Ctrl-C")
    ap.add_argument("--level", type=float, default=0.25, help="amplitude 0..1")
    ap.add_argument("--drift-ppm", type=float, default=0.0, help="make this sender's clock fast (+) or slow (-)")
    ap.add_argument("--loss", type=float, default=0.0, help="probability of dropping each packet, 0..1")
    a = ap.parse_args()
    if not 1 <= a.channels <= 8 or not 1 <= a.frames <= 512:
        ap.error("channels must be 1..8 and frames 1..512")

    fmt = FMT_FLOAT32 if a.format == "float32" else FMT_INT16
    bps = 4 if fmt == FMT_FLOAT32 else 2
    payload_bytes = a.frames * a.channels * bps
    if payload_bytes > 0xFFFF:
        ap.error("packet too large")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (a.host, a.port)
    period = a.frames / (a.rate * (1.0 + a.drift_ppm * 1e-6))
    start = nxt = time.perf_counter()
    seq = frame0 = sent = dropped = 0
    freqs = [a.freq * (1.0 + 0.5 * c) for c in range(a.channels)]       # ch0 = freq, ch1 = 1.5x, ...

    try:
        while a.seconds <= 0 or time.perf_counter() - start < a.seconds:
            samples = []
            for i in range(a.frames):
                for c in range(a.channels):
                    samples.append(a.level * math.sin(2.0 * math.pi * freqs[c] * (frame0 + i) / a.rate))
            if fmt == FMT_FLOAT32:
                payload = struct.pack("<%df" % len(samples), *samples)
            else:
                payload = struct.pack("<%dh" % len(samples), *[int(round(max(-1.0, min(1.0, v)) * 32767)) for v in samples])

            if random.random() >= a.loss:
                sock.sendto(HEADER.pack(MAGIC, VERSION, TYPE_AUDIO, seq & 0xFFFF, frame0 & 0xFFFFFFFF,
                                        payload_bytes, a.channels, fmt) + payload, dest)
                sent += 1
            else:
                dropped += 1

            seq += 1; frame0 += a.frames
            nxt += period
            delay = nxt - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
    except KeyboardInterrupt:
        pass
    print(f"sent {sent} packets ({dropped} deliberately dropped) to {a.host}:{a.port}")

if __name__ == "__main__":
    main()
