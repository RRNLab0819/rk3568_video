#!/usr/bin/env python3
"""Capture chessboard images from all 4 cameras on the RK3568 board.
Usage: python3 grab_chess.py <output_dir>
Captures NUM_IMAGES frames per camera, saved as cam{N}_frame{M}.nv12
"""
import subprocess, sys, os, time

NUM_IMAGES = 25
DEVICES = ['/dev/video0', '/dev/video1', '/dev/video2', '/dev/video3']

def run(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)

out = sys.argv[1] if len(sys.argv) > 1 else '/tmp/chess'
os.makedirs(out, exist_ok=True)

# Push grab_frame tool if not already on board
grab_bin = '/home/rrn/rk3568-camera/tools/grab_frame'
if os.path.exists(grab_bin):
    run(f'adb push {grab_bin} /userdata/grab_frame')
    run('adb shell chmod +x /userdata/grab_frame')

for dev in DEVICES:
    cam_id = dev[-1]
    print(f'\n=== Camera {cam_id}: print chessboard, put in view, press Enter ===')
    print(f'    Will capture {NUM_IMAGES} frames. Move chessboard between frames.')
    input('    Press Enter when ready...')

    for n in range(NUM_IMAGES):
        remote = f'/tmp/chess_cam{cam_id}_{n:02d}.nv12'
        local  = f'{out}/cam{cam_id}_frame{n:02d}.nv12'

        r = run(f'adb shell "/userdata/grab_frame {dev} {remote} 1"')
        if 'error' in r.stderr.lower() or 'busy' in r.stderr.lower():
            # Device may be held; kill stale processes
            run('adb shell "killall -9 rk3568_camera grab_frame 2>/dev/null"')
            time.sleep(1)
            r = run(f'adb shell "/userdata/grab_frame {dev} {remote} 1"')

        run(f'adb pull {remote} {local}')
        size = os.path.getsize(local) if os.path.exists(local) else 0
        expected = 1920 * 1080 * 3 // 2
        print(f'  [{n+1}/{NUM_IMAGES}] {local} ({size} bytes, {"OK" if size==expected else "FAIL"})')
        run(f'adb shell "rm {remote}"')
        time.sleep(0.3)

print('\n=== Done ===')
print(f'Images saved to {out}/')
print('Next: run chessboard_calibrate.py to compute camera intrinsics')
