"""Shared helpers for the OV5647 lens-calibration tools.

Sensor coordinates (see docs/lens-calibration.md): OV5647 pixel-array addresses
in full-resolution pixels, continuous "edge" convention (array column i spans
[i, i+1)). The device reports, for the active mode, a ``window`` object:

    X = window.x + window.width  - u * scale   if window.mirror else window.x + u * scale
    Y = window.y + window.height - v * scale   if window.flip   else window.y + v * scale

where (u, v) are output-image coordinates in the same edge convention. OpenCV
uses pixel-centre-at-integer coordinates, so u_edge = u_opencv + 0.5.
"""
import json
import math


def load_camera(info_path, index=0):
    with open(info_path, "r", encoding="utf-8") as f:
        info = json.load(f)
    cams = info["cameras"] if "cameras" in info else [info]
    for cam in cams:
        if cam.get("index", 0) == index:
            return cam
    raise SystemExit(f"camera index {index} not found in {info_path}")


def window_of(cam):
    win = cam.get("window")
    if not win:
        raise SystemExit("camera info has no 'window' object - is the firmware lens-centre capable?")
    return win


def image_to_sensor(u_cv, v_cv, win):
    """OpenCV image coordinates (pixel centres at integers) -> sensor coordinates."""
    s = win["scale"]
    ue, ve = u_cv + 0.5, v_cv + 0.5
    x = win["x"] + win["width"] - ue * s if win["mirror"] else win["x"] + ue * s
    y = win["y"] + win["height"] - ve * s if win["flip"] else win["y"] + ve * s
    return x, y


def sensor_to_image(x, y, win):
    """Sensor coordinates -> OpenCV image coordinates (pixel centres at integers)."""
    s = win["scale"]
    ue = (win["x"] + win["width"] - x) / s if win["mirror"] else (x - win["x"]) / s
    ve = (win["y"] + win["height"] - y) / s if win["flip"] else (y - win["y"]) / s
    return ue - 0.5, ve - 0.5


def intrinsics_image_to_sensor(K, dist, win):
    """Convert an OpenCV camera matrix + 5 distortion coefficients estimated on
    output images of `win` into the sensor-coordinate model used by the device.

    Mirroring x flips the sign of the tangential coefficient p2, flipping y
    flips p1 (the radial terms are symmetric)."""
    s = win["scale"]
    fx, fy, cx, cy = K[0][0], K[1][1], K[0][2], K[1][2]
    k1, k2, p1, p2, k3 = [float(v) for v in list(dist)[:5]] + [0.0] * (5 - len(list(dist)[:5]))
    X, Y = image_to_sensor(cx, cy, win)
    return {
        "center": {"x": X, "y": Y},
        "distortion": {
            "fx": fx * s,
            "fy": fy * s,
            "k1": k1,
            "k2": k2,
            "p1": -p1 if win["flip"] else p1,
            "p2": -p2 if win["mirror"] else p2,
            "k3": k3,
        },
    }


def intrinsics_sensor_to_image(cal, win):
    """Inverse of intrinsics_image_to_sensor: returns (K, dist) for output images of `win`."""
    s = win["scale"]
    d = cal["distortion"]
    cx, cy = sensor_to_image(cal["center"]["x"], cal["center"]["y"], win)
    K = [[d["fx"] / s, 0.0, cx], [0.0, d["fy"] / s, cy], [0.0, 0.0, 1.0]]
    dist = [d["k1"], d["k2"], -d["p1"] if win["flip"] else d["p1"], -d["p2"] if win["mirror"] else d["p2"], d["k3"]]
    return K, dist


def check_finite(obj):
    for k, v in obj.items():
        if isinstance(v, dict):
            check_finite(v)
        elif isinstance(v, (int, float)) and not math.isfinite(v):
            raise SystemExit(f"non-finite value for {k}")
