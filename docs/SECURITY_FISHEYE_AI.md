# Security Fisheye AI Mode

This branch turns the calibrated four-camera fisheye prototype into a security-oriented AI monitor instead of a vehicle AVM mockup.

## Goal

- Use four calibrated fisheye cameras as a security monitoring product.
- Keep the stable 2x2 AI camera path untouched.
- Avoid fake vehicle surround-view stitching until true extrinsics/IPM are implemented.

## Startup

On the RK3568 board:

`sh
cd /userdata
./start_security.sh
`

The script enables:

- SECURITY_MODE=1
- FISHEYE_CALIB_DIR=/userdata/calib
- four-camera AI inference
- encoder disabled by default

It requires:

`	ext
/userdata/calib/calib_video0.yaml
/userdata/calib/calib_video1.yaml
/userdata/calib/calib_video2.yaml
/userdata/calib/calib_video3.yaml
/userdata/yolov5.rknn
`

## Current UI

The first product UI is a calibrated 2x2 security monitor:

- each camera is rendered through the fisheye dewarping mesh;
- each tile has a status strip;
- person boxes are remapped into the dewarped view;
- box/status colors indicate approximate risk distance.

Colors:

- red: estimated person distance <= SECURITY_WARN_NEAR_M
- amber: estimated person distance <= SECURITY_WARN_MID_M
- green: farther or no reliable distance

## Distance Roadmap

Current distance is a temporary human-height estimate:

`	ext
distance ~= assumed_person_height * focal / bbox_height
`

This is useful for product demos, but it is not the final ranging method.

The final method is the ground-plane foot-point route:

1. detect a person;
2. take the bottom center of the person box as the foot point;
3. undistort the point using fisheye intrinsics;
4. cast a ray using camera extrinsics;
5. intersect the ray with the ground plane;
6. report distance and warning zone.

/userdata/calib/security_extrinsics.ini is created as the future external calibration entry point.

## Orientation

Raw captures may look mirrored or rotated while display looks correct. Security mode keeps orientation controlled by the same runtime variables used by the fisheye mesh:

`sh
FISHEYE_ROTATE=0,0,0,0
FISHEYE_FLIPX=0,0,0,0
FISHEYE_FLIPY=0,0,0,0
`

These values must stay consistent across display, detection remapping and future distance estimation.
