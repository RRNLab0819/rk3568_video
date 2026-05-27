#!/usr/bin/env python3
"""Check that an NV12 file has non-uniform content (not all-gray padding)."""
import sys

def check_nv12(path, w, h):
    with open(path, 'rb') as f:
        data = f.read()
    expected = w * h * 3 // 2
    print(f"File: {path}")
    print(f"Size: {len(data)} bytes (expected {expected})")
    if len(data) < expected:
        print(f"ERROR: File too small: {len(data)} < {expected}")
        return 1

    y = data[:w * h]
    unique = len(set(y))
    gray_count = sum(1 for b in y if b == 128)
    print(f"Y plane: {w}x{h}, unique values: {unique}, gray(128) pixels: {gray_count}/{w * h}")
    if gray_count > w * h * 0.9:
        print("WARNING: >90% gray pixels — output may be blank letterbox")
        print("         (expected if camera shows uniform scene)")
    else:
        print("OK: sufficient non-gray content")
    return 0

if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('-W', type=int, default=320)
    ap.add_argument('-H', type=int, default=320)
    args = ap.parse_args()
    sys.exit(check_nv12(args.path, args.W, args.H))
