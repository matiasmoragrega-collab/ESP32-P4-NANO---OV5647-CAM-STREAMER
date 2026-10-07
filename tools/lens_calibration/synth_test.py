#!/usr/bin/env python3
"""Self-test for calibrate.py on a synthetic dataset.

Renders a checkerboard through a KNOWN lens model (defined in sensor coordinates,
with an off-centre principal point and barrel distortion) at the device's
1280x960 window mapping (scale 2, mirrored), runs the calibration and checks
that the sensor-space centre / focal length / distortion are recovered. Also
renders a vignetted flat field to check the --vignetting estimator.

  python synth_test.py [--out synth] [--views 18]
"""
import argparse
import json
import os
import subprocess
import sys

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lenscal_common import intrinsics_sensor_to_image, sensor_to_image  # noqa: E402

# Ground truth, sensor coordinates (active-area centre is 1312, 978).
TRUTH = {
    "center": {"x": 1340.5, "y": 962.25},
    "distortion": {"fx": 1500.0, "fy": 1497.0, "k1": -0.25, "k2": 0.06, "p1": 0.0008, "p2": -0.0012, "k3": -0.005},
}
# Window of the 1280x960 mode at the default lens centre, as reported by the device.
WINDOW = {"x": 32, "y": 18, "width": 2560, "height": 1920, "scale": 2, "mirror": True, "flip": False,
          "centerError": {"x": 0, "y": 0}, "centerRange": {"xMin": 1296, "xMax": 1328, "yMin": 966, "yMax": 990}}
W, H = 1280, 960
BOARD = (9, 6)          # inner corners
SQUARE = 30.0           # mm


def camera_info():
    return {"cameras": [{
        "index": 0, "currentImageFormat": 4, "currentResolution": {"width": W, "height": H},
        "sensor": {"arrayWidth": 2624, "arrayHeight": 1956, "activeArea": {"x": 16, "y": 6, "width": 2592, "height": 1944}},
        "lensCalibration": {"center": {"x": 1312, "y": 978}, "source": "default", "distortion": None},
        "window": WINDOW}]}


def render_view(K, dist, rvec, tvec, ss=2):
    """Ray-trace the board: for every (super-sampled) output pixel, undistort to a ray and
    intersect the board plane."""
    us, vs = np.meshgrid((np.arange(W * ss) + 0.5) / ss - 0.5, (np.arange(H * ss) + 0.5) / ss - 0.5)
    pts = np.stack([us.ravel(), vs.ravel()], 1).astype(np.float64).reshape(-1, 1, 2)
    crit = (cv2.TERM_CRITERIA_COUNT | cv2.TERM_CRITERIA_EPS, 200, 1e-12)
    und = cv2.undistortPoints(pts, K, dist, None, np.eye(3), np.eye(3), crit).reshape(-1, 2)
    # reject pixels where the inverse did not converge
    back, _ = cv2.projectPoints(np.hstack([und, np.ones((len(und), 1))]).reshape(-1, 1, 3), np.zeros(3), np.zeros(3), K, dist)
    bad = np.linalg.norm(back.reshape(-1, 2) - pts.reshape(-1, 2), axis=1) > 0.05
    R, _ = cv2.Rodrigues(rvec)
    n, t = R[:, 2], tvec.ravel()
    d = np.hstack([und, np.ones((len(und), 1))])
    lam = (n @ t) / (d @ n)
    P = d * lam[:, None]
    B = (P - t) @ R            # board coords (R^T (P - t))
    bx, by = B[:, 0] / SQUARE, B[:, 1] / SQUARE
    cols, rows = BOARD[0] + 1, BOARD[1] + 1
    inside = (bx >= -1) & (bx < cols - 1) & (by >= -1) & (by < rows - 1) & (lam > 0)
    border = (bx >= -1.6) & (bx < cols - 0.4) & (by >= -1.6) & (by < rows - 0.4) & (lam > 0)
    val = np.full(len(bx), 90.0)                       # background
    val[border] = 235.0                                # white margin around the board
    black = ((np.floor(bx) + np.floor(by)) % 2 == 0)
    val[inside & black] = 25.0
    val[bad] = 90.0
    img = val.reshape(H * ss, W * ss)
    img = cv2.resize(img, (W, H), interpolation=cv2.INTER_AREA)
    return img


def make_views(K, dist, n, rng):
    objp = np.zeros((BOARD[0] * BOARD[1], 3))
    objp[:, :2] = np.mgrid[0:BOARD[0], 0:BOARD[1]].T.reshape(-1, 2) * SQUARE
    centre = np.array([(BOARD[0] - 1) * SQUARE / 2, (BOARD[1] - 1) * SQUARE / 2, 0])
    views = []
    while len(views) < n:
        ang = np.deg2rad(rng.uniform(-40, 40, 3)) * np.array([1, 1, 0.5])
        R, _ = cv2.Rodrigues(ang)
        z = rng.uniform(260, 520)
        # aim at a random image point so the board covers centre, edges and corners
        u, v = rng.uniform(0.12, 0.88) * W, rng.uniform(0.12, 0.88) * H
        ray = cv2.undistortPoints(np.array([[[u, v]]], np.float64), K, dist).ravel()
        tvec = np.array([ray[0] * z, ray[1] * z, z]) - R @ centre
        rvec, _ = cv2.Rodrigues(R)
        proj, _ = cv2.projectPoints(objp, rvec, tvec, K, dist)
        proj = proj.reshape(-1, 2)
        if (proj[:, 0].min() < 25 or proj[:, 1].min() < 25 or proj[:, 0].max() > W - 25 or proj[:, 1].max() > H - 25):
            continue
        if (R @ np.array([0, 0, 1.0]))[2] > -0.5 and (R @ np.array([0, 0, 1.0]))[2] < 0.5:
            continue
        views.append((rvec, tvec))
    return views


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(HERE, "synth"))
    ap.add_argument("--views", type=int, default=18)
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    K, dist = intrinsics_sensor_to_image(TRUTH, WINDOW)
    K, dist = np.array(K), np.array(dist)
    print("ground truth, image space: fx=%.2f fy=%.2f cx=%.3f cy=%.3f dist=%s" % (K[0, 0], K[1, 1], K[0, 2], K[1, 2], dist))
    with open(os.path.join(args.out, "camera_info.json"), "w") as f:
        json.dump(camera_info(), f, indent=1)
    for i, (rvec, tvec) in enumerate(make_views(K, dist, args.views, rng)):
        img = render_view(K, dist, rvec, tvec)
        img = np.clip(img + rng.normal(0, 2.0, img.shape), 0, 255).astype(np.uint8)
        cv2.imwrite(os.path.join(args.out, f"frame_{i:03d}.jpg"), cv2.cvtColor(img, cv2.COLOR_GRAY2BGR),
                    [cv2.IMWRITE_JPEG_QUALITY, 92])
        print(f"rendered view {i}", flush=True)

    # flat field with cos^4-like falloff centred on the true principal point
    us, vs = np.meshgrid(np.arange(W), np.arange(H))
    cu, cv_ = sensor_to_image(TRUTH["center"]["x"], TRUTH["center"]["y"], WINDOW)
    r2 = ((us - cu) ** 2 + (vs - cv_) ** 2) / K[0, 0] ** 2
    flat = 200.0 / (1 + r2) ** 2
    flat = np.clip(flat + rng.normal(0, 1.5, flat.shape), 0, 255).astype(np.uint8)
    flat_dir = os.path.join(args.out, "flat")
    os.makedirs(flat_dir, exist_ok=True)
    cv2.imwrite(os.path.join(flat_dir, "flat.png"), cv2.cvtColor(flat, cv2.COLOR_GRAY2BGR))

    py = sys.executable
    subprocess.run([py, os.path.join(HERE, "calibrate.py"), args.out, "--board", f"{BOARD[0]}x{BOARD[1]}",
                    "--square", str(SQUARE)], check=True)
    got = json.load(open(os.path.join(args.out, "calibration.json")))
    print("\n=== recovered vs truth (sensor coordinates) ===")
    ok = True
    tol = {"x": 1.0, "y": 1.0, "fx": 3.0, "fy": 3.0, "k1": 0.005, "k2": 0.01, "p1": 2e-4, "p2": 2e-4, "k3": 0.01}
    for k in ("x", "y"):
        e = got["center"][k] - TRUTH["center"][k]
        ok &= abs(e) <= tol[k]
        print(f"centre.{k}: {got['center'][k]:10.3f}  truth {TRUTH['center'][k]:10.3f}  err {e:+.3f}")
    for k, v in TRUTH["distortion"].items():
        e = got["distortion"][k] - v
        ok &= abs(e) <= tol[k]
        print(f"{k:>8}: {got['distortion'][k]:10.5f}  truth {v:10.5f}  err {e:+.5f}")
    subprocess.run([py, os.path.join(HERE, "calibrate.py"), "--vignetting", os.path.join(flat_dir, "flat.png"),
                    "--info", os.path.join(args.out, "camera_info.json")], check=True)
    print("vignetting truth (sensor coords): x=%.1f y=%.1f" % (TRUTH["center"]["x"], TRUTH["center"]["y"]))
    print("\nSELF-TEST " + ("PASSED" if ok else "FAILED"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
