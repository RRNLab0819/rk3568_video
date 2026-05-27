#!/usr/bin/env python3
"""Offline fisheye undistort verification using factory calibration parameters.

Parses calibinfo.lua (camera intrinsic/extrinsic) and lens.lua (distortion table),
generates an inverse-mapping UV mesh, and remaps a raw NV12 frame to produce
a visually verifiable undistorted output image.

Usage:
  python3 fisheye_verify.py calibinfo.lua lens.lua cam0.nv12 0 output.png
"""

import sys, re, struct, os
import numpy as np
from collections import OrderedDict

# ---------------------------------------------------------------------------
# Lua table parser — handles the exact format of calibinfo.lua / lens.lua
# ---------------------------------------------------------------------------

def lua_parse_float_array(s):
    """Parse '{ 1.0, 2.5, -3.0, }' -> list of floats."""
    vals = []
    for m in re.finditer(r'([+-]?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)', s):
        vals.append(float(m.group(1)))
    return vals

def lua_find_block(text, key):
    """Find a Lua table block starting after 'key = ' or 'key = {'.
    Returns (start_pos, content_inside_braces) or (None, None)."""
    # Match "KeyName = {" or "KeyName = value"
    pattern = re.compile(r'\b' + re.escape(key) + r'\s*=\s*\{')
    m = pattern.search(text)
    if not m:
        return None, None
    start = m.start()
    pos = m.end() - 1  # points to '{'

    # Track braces to find matching closing brace
    depth = 0
    end = pos
    for i in range(pos, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    return start, text[pos:end]

def lua_parse_cameras(text):
    """Parse the 'Cameras' array from calibinfo.lua.
    Returns list of camera dicts with Internal, External, MajorPattern, MinorPattern."""

    _, block = lua_find_block(text, "Cameras")
    if not block:
        print("ERROR: 'Cameras = {' not found")
        return []

    # Split into individual camera blocks (each enclosed in { ... },)
    cameras = []
    depth = 0
    cam_start = -1
    for i, ch in enumerate(block):
        if ch == '{':
            depth += 1
            if depth == 2:  # camera block starts (depth 1 is the array itself)
                cam_start = i
        elif ch == '}':
            if depth == 2 and cam_start >= 0:
                cam_block = block[cam_start:i+1]
                cameras.append(cam_block)
                cam_start = -1
            depth -= 1

    result = []
    for cam_block in cameras:
        cam = {}

        # Parse Internal block
        _, intr_block = lua_find_block(cam_block, "Internal")
        if intr_block:
            intr = {}
            for key in ['Center', 'DefaultCenter', 'PixelSize']:
                _, b = lua_find_block(intr_block, key)
                if b:
                    intr[key.lower()] = lua_parse_float_array(b)
            for key in ['Focal', 'Scale', 'DefaultFocal', 'DefaultScale']:
                m = re.search(r'\b' + key + r'\s*=\s*([+-]?\d+(?:\.\d+)?)', intr_block)
                if m:
                    intr[key.lower()] = float(m.group(1))
            for key in ['Lens', 'Sensor']:
                m = re.search(r'\b' + key + r'\s*=\s*"([^"]*)"', intr_block)
                if m:
                    intr[key.lower()] = m.group(1)
            m = re.search(r'\bMirror\s*=\s*(true|false)', intr_block)
            if m:
                intr['mirror'] = (m.group(1) == 'true')
            m = re.search(r'DistorParam\s*=\s*\{([^}]*)\}', intr_block)
            if m:
                intr['distorparam'] = lua_parse_float_array(m.group(1))
            cam['internal'] = intr

        # Parse External block
        _, extr_block = lua_find_block(cam_block, "External")
        if extr_block:
            extr = {}
            for key in ['CameraPos', 'CameraUp', 'LookatPos']:
                _, b = lua_find_block(extr_block, key)
                if b:
                    extr[key.lower()] = lua_parse_float_array(b)
            cam['external'] = extr

        # Parse MajorPattern
        _, mp_block = lua_find_block(cam_block, "MajorPattern")
        if mp_block:
            cam['majorpattern'] = lua_parse_float_array(mp_block)

        result.append(cam)

    return result

def lua_parse_lens_table(text, lens_name):
    """Parse a specific lens model's DistortionTable from lens.lua.
    Each lens model: ["NAME"] = { FocalLength = N, DistortionTable = { {a, r0, r1, e}, ... } }
    Returns {'focal_length': float, 'table': [(angle, r_ideal, r_real, error), ...]}
    """
    # Find the lens model by name
    pattern = r'\["' + re.escape(lens_name) + r'"\]\s*=\s*\{'
    m = re.search(pattern, text)
    if not m:
        print(f"ERROR: Lens model '{lens_name}' not found in lens.lua")
        return None

    # Find the enclosing braces
    pos = m.end() - 1
    depth = 0
    start = pos
    for i in range(pos, len(text)):
        if text[i] == '{':
            if depth == 0:
                start = i
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                lens_block = text[start:i+1]
                break

    # Parse FocalLength
    lens = {}
    m = re.search(r'FocalLength\s*=\s*([\d.]+)', lens_block)
    if m:
        lens['focal_length'] = float(m.group(1))

    # Parse DistortionTable
    _, dt_block = lua_find_block(lens_block, "DistortionTable")
    if dt_block:
        table = []
        # Each row: {angle, r_ideal, r_real, error},
        for row_match in re.finditer(r'\{([^}]*)\}', dt_block):
            vals = lua_parse_float_array(row_match.group(1))
            if len(vals) >= 3:
                table.append(tuple(vals[:4]) if len(vals) >= 4 else tuple(vals[:3]) + (0.0,))
        lens['table'] = table

    return lens

# ---------------------------------------------------------------------------
# Mesh generator
# ---------------------------------------------------------------------------

def generate_undistort_mesh(calib, lens_data, output_w, output_h, fov_h=120.0):
    """Generate inverse-mapping mesh for fisheye undistortion.

    Approach: for each output pixel, compute the corresponding source (fisheye) UV.
    Uses the lens distortion table + camera intrinsics for the inverse mapping.

    The factory AVM uses a sphere model: output pixels map to a sphere surface,
    then project through the pinhole camera model with polynomial distortion.

    Returns map_x, map_y (float32 arrays of shape output_h x output_w).
    """
    intr = calib.get('internal', {})

    cx = intr.get('center', [960, 540])[0]
    cy = intr.get('center', [960, 540])[1] if len(intr.get('center', [960, 540])) > 1 else 540
    focal = intr.get('focal', 1400.0)
    distor = intr.get('distorparam', [13.08, 35.63, -2.26, -4.94])
    src_w = intr.get('pixelsize', [1920, 1080])[0]
    src_h = intr.get('pixelsize', [1920, 1080])[1] if len(intr.get('pixelsize', [1920, 1080])) > 1 else 1080

    fov_v = fov_h * output_h / output_w
    fov_h_rad = np.radians(fov_h)
    fov_v_rad = np.radians(fov_v)

    # Build angle->r lookup from lens distortion table if available
    angle_to_r = None
    if lens_data and 'table' in lens_data and lens_data['table']:
        tbl = lens_data['table']
        angles = np.array([row[0] for row in tbl], dtype=np.float64)
        r_vals = np.array([row[2] if len(row) > 2 else row[1] for row in tbl], dtype=np.float64)
        # r in the table is normalized (r / focal_length)
        # Convert to pixel radius: r_pixel = r_normalized * focal
        r_pixel = r_vals * focal
        # Only use monotonic forward part (before r starts decreasing)
        mono_end = 1
        for i in range(1, len(r_pixel)):
            if r_pixel[i] <= r_pixel[i-1]:
                mono_end = i
                break
            mono_end = i + 1
        angle_to_r = (np.radians(angles[:mono_end]), r_pixel[:mono_end])

    # Grid of output coords
    y_out, x_out = np.mgrid[0:output_h, 0:output_w]

    # Map output coords to sphere angles
    theta = (x_out / output_w - 0.5) * fov_h_rad  # horizontal angle
    phi = (y_out / output_h - 0.5) * fov_v_rad     # vertical angle

    # Sphere → pinhole projection:
    # On a unit sphere at distance R:
    #   X = R * tan(theta) (in camera X axis)
    #   Y = R * tan(phi) / cos(theta) (in camera Y axis)
    #   Z = R (forward)
    # Projection: u = fx * X/Z + cx, v = fy * Y/Z + cy

    tan_theta = np.tan(theta)
    tan_phi = np.tan(phi)
    cos_theta = np.cos(theta)

    u_ideal = focal * tan_theta + cx
    v_ideal = focal * tan_phi / cos_theta + cy

    # Compute incident angle for each output pixel (from optical axis)
    dx = u_ideal - cx
    dy = v_ideal - cy
    r_ideal = np.sqrt(dx*dx + dy*dy)
    incident_angle = np.arctan2(r_ideal, focal)  # radians

    # Apply lens distortion: ideal → real radius
    if angle_to_r is not None:
        # Use lens table lookup
        ang_rad, r_tbl = angle_to_r
        r_distorted = np.interp(incident_angle, ang_rad, r_tbl, left=0, right=r_tbl[-1]*2)
    else:
        # Polynomial model: r_distorted = r_ideal * (1 + k1*r² + k2*r⁴ + k3*r⁶ + k4*r⁸)
        r2 = (r_ideal / focal) ** 2
        r4 = r2 * r2
        r6 = r4 * r2
        r8 = r4 * r4
        k = distor
        scale = 1.0 + k[0]*r2 + k[1]*r4 + k[2]*r6 + k[3]*r8
        r_distorted = r_ideal * scale

    # Reconstruct distorted pixel coordinates
    # Direction from optical center stays the same; only radius changes
    mask = (r_ideal > 1e-9)
    u_src = np.where(mask, cx + dx * (r_distorted / np.maximum(r_ideal, 1e-9)), cx)
    v_src = np.where(mask, cy + dy * (r_distorted / np.maximum(r_ideal, 1e-9)), cy)

    # Also where incident angle is 0, use center directly
    u_src[~mask] = cx
    v_src[~mask] = cy

    # Clamp to valid image range
    u_src = np.clip(u_src, 0, src_w - 1)
    v_src = np.clip(v_src, 0, src_h - 1)

    return u_src.astype(np.float32), v_src.astype(np.float32)

# ---------------------------------------------------------------------------
# NV12 reader
# ---------------------------------------------------------------------------

def read_nv12(path, width, height):
    """Read a raw NV12 file and return BGR image."""
    with open(path, 'rb') as f:
        data = f.read()

    expected = width * height * 3 // 2
    if len(data) < expected:
        raise ValueError(f"File too small: {len(data)} < {expected} (need {width}x{height} NV12)")

    y  = np.frombuffer(data[:width*height], dtype=np.uint8).reshape(height, width)
    uv = np.frombuffer(data[width*height:expected], dtype=np.uint8).reshape(height//2, width)

    # NV12 → BGR via OpenCV
    import cv2
    yuv = cv2.cvtColor(cv2.merge([y, uv]), cv2.COLOR_YUV2BGR_NV12)
    return yuv

def read_nv12_python(path, width, height):
    """Pure-python NV12→RGB (fallback, no OpenCV required)."""
    with open(path, 'rb') as f:
        data = f.read()

    y_sz = width * height
    y  = np.frombuffer(data[:y_sz], dtype=np.uint8).reshape(height, width)
    uv = np.frombuffer(data[y_sz:y_sz*3//2], dtype=np.uint8).reshape(height//2, width)

    # Upsample chroma
    import cv2
    u_up = cv2.resize(uv[:, ::2], (width, height), interpolation=cv2.INTER_LINEAR)
    v_up = cv2.resize(uv[:, 1::2], (width, height), interpolation=cv2.INTER_LINEAR)

    # YUV→RGB (BT.601 limited range)
    y_f = y.astype(np.float32)
    u_f = u_up.astype(np.float32) - 128.0
    v_f = v_up.astype(np.float32) - 128.0

    r = np.clip(y_f + 1.402 * v_f, 0, 255).astype(np.uint8)
    g = np.clip(y_f - 0.344136 * u_f - 0.714136 * v_f, 0, 255).astype(np.uint8)
    b = np.clip(y_f + 1.772 * u_f, 0, 255).astype(np.uint8)

    return cv2.merge([b, g, r])

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    import argparse
    ap = argparse.ArgumentParser(
        description='Offline fisheye undistort verification using factory calibration')
    ap.add_argument('calibinfo', help='Path to calibinfo.lua')
    ap.add_argument('lens', help='Path to lens.lua')
    ap.add_argument('nv12_frame', help='Path to raw NV12 frame (1920x1080)')
    ap.add_argument('cam_index', type=int, default=0, help='Camera index (0-3)')
    ap.add_argument('output', help='Output image path (.png)')
    ap.add_argument('--fov', type=float, default=120.0, help='Horizontal FOV (default 120)')
    ap.add_argument('--out-size', type=int, default=480, help='Output resolution (square)')
    ap.add_argument('--compare', action='store_true',
                    help='Output side-by-side comparison (original vs undistorted)')
    args = ap.parse_args()

    # Parse calibration
    with open(args.calibinfo, 'r') as f:
        calib_text = f.read()

    cameras = lua_parse_cameras(calib_text)
    print(f"Parsed {len(cameras)} cameras from {args.calibinfo}")

    if args.cam_index >= len(cameras):
        print(f"ERROR: cam_index {args.cam_index} >= {len(cameras)} cameras")
        sys.exit(1)

    cam = cameras[args.cam_index]
    intr = cam.get('internal', {})
    extr = cam.get('external', {})

    print(f"\nCamera {args.cam_index}:")
    print(f"  Center:      {intr.get('center', 'N/A')}")
    print(f"  Focal:       {intr.get('focal', 'N/A')}")
    print(f"  Scale:       {intr.get('scale', 'N/A')}")
    print(f"  DistorParam: {intr.get('distorparam', 'N/A')}")
    print(f"  Lens:        {intr.get('lens', 'N/A')}")
    print(f"  Sensor:      {intr.get('sensor', 'N/A')}")
    if extr:
        print(f"  CameraPos:   {extr.get('camerapos', 'N/A')}")
        print(f"  LookatPos:   {extr.get('lookatpos', 'N/A')}")

    # Parse lens distortion table
    lens_name = intr.get('lens', '6028')
    with open(args.lens, 'r') as f:
        lens_text = f.read()

    lens_data = lua_parse_lens_table(lens_text, lens_name)
    if lens_data:
        print(f"\nLens '{lens_name}': FocalLength={lens_data.get('focal_length')}, "
              f"Table rows={len(lens_data.get('table', []))}")
    else:
        print(f"\nWARNING: Lens '{lens_name}' not found, using polynomial model only")

    # Read frame
    print(f"\nReading {args.nv12_frame}...")
    import cv2
    try:
        img = read_nv12(args.nv12_frame, 1920, 1080)
    except Exception as e:
        print(f"  OpenCV NV12 failed ({e}), trying python fallback...")
        img = read_nv12_python(args.nv12_frame, 1920, 1080)
    print(f"  Image shape: {img.shape}")

    # Generate undistort mesh
    out_w, out_h = args.out_size, args.out_size
    print(f"\nGenerating undistort mesh ({out_w}x{out_h}, FOV={args.fov}°)...")
    map_x, map_y = generate_undistort_mesh(cam, lens_data, out_w, out_h, args.fov)

    # Apply remap
    print("Remapping...")
    result = cv2.remap(img, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=(0,0,0))

    # Output
    if args.compare:
        # Side-by-side: original (center crop) | undistorted
        orig_crop = img[140:940, 480:1440]  # center crop of original
        orig_resized = cv2.resize(orig_crop, (out_w, out_h))
        combined = np.hstack([orig_resized, result])
        cv2.imwrite(args.output, combined)
    else:
        cv2.imwrite(args.output, result)

    print(f"Saved {args.output}")

    # Quick stats
    valid_mask = (map_x > 0) & (map_x < 1919) & (map_y > 0) & (map_y < 1079)
    valid_pct = np.sum(valid_mask) / (out_w * out_h) * 100
    print(f"Valid pixel coverage: {valid_pct:.1f}%")

if __name__ == '__main__':
    main()
