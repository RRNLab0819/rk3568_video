#!/usr/bin/env python3
"""Calibrate fisheye cameras using chessboard frames.
Reads NV12 frames captured by grab_frame, detects chessboard corners,
and computes per-camera intrinsic parameters (K, dist, rms).

Usage:
  python3 calibrate_chess.py ./chess_frames/cam0/ -p 9x6 -s 25
  Outputs calibrated camera parameters for use in fisheye_mesh.c
"""

import cv2, numpy as np, os, sys, glob, argparse, json

def read_nv12(path, w=1920, h=1080):
    with open(path, 'rb') as f:
        data = f.read()
    expected = w * h * 3 // 2
    if len(data) < expected:
        return None
    # NV12: Y plane (h×w) followed by interleaved UV (h/2 × w)
    yuv = np.frombuffer(data[:expected], dtype=np.uint8).reshape(h + h//2, w)
    bgr = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV12)
    return bgr

def calibrate_camera(image_dir, pattern_size, square_mm=25.0):
    """Calibrate one camera from chessboard images.
    Returns: (camera_matrix, dist_coeffs, rms, img_size, success_count)
    """
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.001)

    objp = np.zeros((pattern_size[0] * pattern_size[1], 3), np.float32)
    objp[:, :2] = np.mgrid[0:pattern_size[0], 0:pattern_size[1]].T.reshape(-1, 2)
    objp *= square_mm

    objpoints = []
    imgpoints = []
    img_size = None
    success_files = []
    fail_files = []

    files = sorted(glob.glob(os.path.join(image_dir, '*.nv12')))
    print(f"Processing {len(files)} images from {image_dir}...")

    for fpath in files:
        img = read_nv12(fpath)
        if img is None:
            fail_files.append((fpath, 'read error'))
            continue

        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        img_size = (gray.shape[1], gray.shape[0])

        ret, corners = cv2.findChessboardCorners(gray, pattern_size, None)
        if ret:
            corners2 = cv2.cornerSubPix(gray, corners, (11, 11), (-1, -1), criteria)
            objpoints.append(objp)
            imgpoints.append(corners2)
            success_files.append(fpath)
        else:
            fail_files.append((fpath, 'no chessboard found'))

    if len(objpoints) < 5:
        return None, None, None, img_size, 0, success_files, fail_files

    # Standard pinhole + distortion model
    rms_std, K_std, dist_std, rvecs, tvecs = cv2.calibrateCamera(
        objpoints, imgpoints, img_size, None, None)

    # Fisheye model (Kannala-Brandt)
    try:
        rms_fish, K_fish, dist_fish, _, _ = cv2.fisheye.calibrate(
            objpoints, imgpoints, img_size, None, None,
            flags=cv2.fisheye.CALIB_RECOMPUTE_EXTRINSIC |
                  cv2.fisheye.CALIB_FIX_SKEW)
        fish_ok = True
    except:
        K_fish, dist_fish = None, None
        rms_fish = -1
        fish_ok = False

    return (K_std, dist_std, rms_std,
            K_fish, dist_fish, rms_fish, fish_ok,
            img_size, len(objpoints), success_files, fail_files)

def main():
    ap = argparse.ArgumentParser(description='Calibrate fisheye cameras from chessboard NV12 frames')
    ap.add_argument('input_dir', help='Directory containing cam*_frame*.nv12 files')
    ap.add_argument('-p', '--pattern', default='9x6', help='Chessboard inner corners WxH (default: 9x6)')
    ap.add_argument('-s', '--square', type=float, default=25.0, help='Square size in mm (default: 25)')
    ap.add_argument('--cam', type=int, default=-1, help='Single camera index to calibrate (-1 = all)')
    ap.add_argument('-o', '--output', default=None, help='Output JSON file for calibration params')
    args = ap.parse_args()

    w, h = map(int, args.pattern.split('x'))
    pattern = (w, h)

    # Auto-detect per-camera directories or single directory
    base = args.input_dir
    cam_dirs = {}
    if args.cam >= 0:
        cam_dirs[args.cam] = base
    else:
        # Check if base contains cam subdirectories
        for i in range(4):
            d = os.path.join(base, f'cam{i}')
            if os.path.isdir(d):
                cam_dirs[i] = d
        if not cam_dirs:
            # Single directory with all frames — split by filename pattern
            cam_dirs[0] = base

    all_results = {}
    for cam_idx, cam_dir in sorted(cam_dirs.items()):
        if args.cam >= 0 and cam_idx != args.cam:
            # When single camera specified via --cam, try matching frames
            files = sorted(glob.glob(os.path.join(base, f'cam{args.cam}_frame*.nv12')))
            if files:
                pass

        print(f"\n{'='*60}")
        print(f"Camera {cam_idx}")
        print(f"{'='*60}")

        (K_std, dist_std, rms_std,
         K_fish, dist_fish, rms_fish, fish_ok,
         size, n_ok, ok_files, fail_files) = calibrate_camera(cam_dir, pattern, args.square)

        if K_std is None:
            print(f"  FAILED: only {n_ok} good images (need >=5)")
            for f, reason in fail_files:
                print(f"    FAIL: {os.path.basename(f)} - {reason}")
            continue

        print(f"  Image size: {size}")
        print(f"  Good frames: {n_ok}/{n_ok + len(fail_files)}")
        print()
        print(f"  --- Standard (pinhole + distortion) ---")
        print(f"  RMS: {rms_std:.4f} px")
        print(f"  K: fx={K_std[0,0]:.3f} fy={K_std[1,1]:.3f} cx={K_std[0,2]:.3f} cy={K_std[1,2]:.3f}")
        print(f"  dist: k1={dist_std[0,0]:.6f} k2={dist_std[0,1]:.6f} "
              f"p1={dist_std[0,2]:.6f} p2={dist_std[0,3]:.6f} k3={dist_std[0,4]:.6f}")
        if fish_ok:
            print()
            print(f"  --- Fisheye (Kannala-Brandt) ---")
            print(f"  RMS: {rms_fish:.4f} px")
            print(f"  K: fx={K_fish[0,0]:.3f} fy={K_fish[1,1]:.3f} cx={K_fish[0,2]:.3f} cy={K_fish[1,2]:.3f}")
            print(f"  dist: k1={dist_fish[0,0]:.6f} k2={dist_fish[0,1]:.6f} "
                  f"k3={dist_fish[0,2]:.6f} k4={dist_fish[0,3]:.6f}")
            dist_out = [float(dist_fish[0,i]) for i in range(4)]
        else:
            dist_out = [float(dist_std[0,i]) for i in range(5)]

        all_results[f'cam{cam_idx}'] = {
            'fx': float(K_std[0,0]), 'fy': float(K_std[1,1]),
            'cx': float(K_std[0,2]), 'cy': float(K_std[1,2]),
            'dist_std': [float(dist_std[0,i]) for i in range(5)],
            'dist_fish': dist_out if fish_ok else None,
            'rms_std': float(rms_std),
            'rms_fish': float(rms_fish) if fish_ok else -1,
            'image_count': n_ok,
            'width': size[0], 'height': size[1],
        }

    if args.output:
        with open(args.output, 'w') as f:
            json.dump(all_results, f, indent=2)
        print(f"\nSaved to {args.output}")

    # Print C header format
    print("\n=== C header format (for fisheye_mesh.c) ===")
    print("// Replace g_fisheye_cams[] array with these calibrated values:")
    for cam_name, r in sorted(all_results.items()):
        cam_id = int(cam_name[3:])
        fmean = (r['fx'] + r['fy']) / 2.0
        print(f"    [{cam_id}] = {{ .cx = {r['cx']:.1f}f, .cy = {r['cy']:.1f}f, "
              f".focal = {fmean:.1f}f, .scale = 1.0f, "
              f".src_w = {r['width']}, .src_h = {r['height']} }},")
    print()
    if all_results:
        r0 = list(all_results.values())[0]
        if r0.get('dist_fish'):
            print("// Fisheye distortion coefficients (k1,k2,k3,k4):")
            for cam_name, r in sorted(all_results.items()):
                df = r['dist_fish']
                print(f"// {cam_name}: k1={df[0]:.6f} k2={df[1]:.6f} k3={df[2]:.6f} k4={df[3]:.6f}")

if __name__ == '__main__':
    main()
