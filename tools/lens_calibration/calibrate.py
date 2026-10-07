#!/usr/bin/env python3
"""OpenCV checkerboard calibration of the OV5647 lens, expressed in SENSOR coordinates.

  python calibrate.py shots --board 9x6 [--square 25] [--upload 192.168.137.209]
  python calibrate.py --vignetting flat.jpg --info shots/camera_info.json

The folder must contain the frames and the camera_info.json written by capture.py
(it holds the sensor window of the mode the frames were taken in). The result
(lens centre = principal point, fx, fy, k1, k2, p1, p2, k3 in sensor pixels) is
written to <folder>/calibration.json in exactly the format accepted by
POST /api/set_lens_calibration; details go to <folder>/calibration_report.json.
"""
import argparse
import glob
import json
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lenscal_common import (check_finite, image_to_sensor, intrinsics_image_to_sensor,  # noqa: E402
                            load_camera, window_of)


# ----------------------------------------------------------------------------- checkerboard
def detect(path, board):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    if img is None:
        return None, None
    ok, corners = False, None
    if hasattr(cv2, "findChessboardCornersSB"):
        ok, corners = cv2.findChessboardCornersSB(img, board, flags=cv2.CALIB_CB_NORMALIZE_IMAGE |
                                                  cv2.CALIB_CB_EXHAUSTIVE | cv2.CALIB_CB_ACCURACY)
    if not ok:
        ok, corners = cv2.findChessboardCorners(img, board, flags=cv2.CALIB_CB_ADAPTIVE_THRESH |
                                                cv2.CALIB_CB_NORMALIZE_IMAGE)
        if ok:
            corners = cv2.cornerSubPix(img, corners, (7, 7), (-1, -1),
                                       (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-4))
    return (corners.reshape(-1, 1, 2).astype(np.float32) if ok else None), img.shape[::-1]


def calibrate(args):
    cam = load_camera(os.path.join(args.folder, "camera_info.json"), args.index)
    win = window_of(cam)
    res = cam["currentResolution"]
    cols, rows = (int(v) for v in args.board.lower().split("x"))
    board = (cols, rows)
    objp = np.zeros((cols * rows, 3), np.float32)
    objp[:, :2] = np.mgrid[0:cols, 0:rows].T.reshape(-1, 2) * args.square

    files = sorted(glob.glob(os.path.join(args.folder, "*.jpg")) + glob.glob(os.path.join(args.folder, "*.png")))
    obj_pts, img_pts, used = [], [], []
    size = None
    for f in files:
        corners, sz = detect(f, board)
        if sz is not None and (sz[0] != res["width"] or sz[1] != res["height"]):
            sys.exit(f"{f} is {sz[0]}x{sz[1]} but camera_info says {res['width']}x{res['height']}")
        size = sz or size
        print(f"  {os.path.basename(f)}: {'ok' if corners is not None else 'board NOT found'}")
        if corners is not None:
            obj_pts.append(objp)
            img_pts.append(corners)
            used.append(os.path.basename(f))
    if len(used) < 5:
        sys.exit(f"only {len(used)} usable images - need at least 5 (15+ recommended)")

    flags = 0
    if args.no_k3:
        flags |= cv2.CALIB_FIX_K3
    if args.no_tangential:
        flags |= cv2.CALIB_ZERO_TANGENT_DIST
    crit = (cv2.TERM_CRITERIA_COUNT + cv2.TERM_CRITERIA_EPS, 200, 1e-12)
    rms, K, dist, rvecs, tvecs, std_int, _, per_view = cv2.calibrateCameraExtended(
        obj_pts, img_pts, size, None, None, flags=flags, criteria=crit)
    dist = dist.ravel()[:5]
    std_int = std_int.ravel()

    print(f"\n{len(used)} images, RMS reprojection error {rms:.3f} px (output-image px)")
    worst = sorted(zip(per_view.ravel(), used), reverse=True)[:3]
    print("worst views: " + ", ".join(f"{n} {e:.2f}px" for e, n in worst))
    print(f"image-space: fx={K[0,0]:.2f} fy={K[1,1]:.2f} cx={K[0,2]:.2f}+-{std_int[2]:.2f} "
          f"cy={K[1,2]:.2f}+-{std_int[3]:.2f}  dist={np.array2string(dist, precision=5)}")

    result = intrinsics_image_to_sensor(K, dist, win)
    s = win["scale"]
    print(f"\nwindow: x={win['x']} y={win['y']} {win['width']}x{win['height']} scale={s} "
          f"mirror={win['mirror']} flip={win['flip']}")
    c, d = result["center"], result["distortion"]
    print(f"LENS CENTRE (sensor coords): x={c['x']:.2f} +-{std_int[2]*s:.2f}  y={c['y']:.2f} +-{std_int[3]*s:.2f}")
    act = cam.get("sensor", {}).get("activeArea")
    if act:
        print(f"  offset from active-area centre: dx={c['x'] - (act['x'] + act['width'] / 2):+.2f} "
              f"dy={c['y'] - (act['y'] + act['height'] / 2):+.2f}")
    print("distortion (sensor px): " + ", ".join(f"{k}={v:.6g}" for k, v in d.items()))

    body = {"index": args.index, "center": c, "distortion": d}
    check_finite(body)
    out = os.path.join(args.folder, "calibration.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(body, f, indent=2)
    report = {
        "rms_px": rms, "images": used, "per_view_rms_px": per_view.ravel().tolist(),
        "image_size": list(size), "window": win,
        "image_space": {"K": K.tolist(), "dist": dist.tolist(), "std_intrinsics": std_int[:9].tolist()},
        "sensor_space": body,
        "board": {"inner_corners": [cols, rows], "square": args.square},
    }
    with open(os.path.join(args.folder, "calibration_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\nwrote {out}")
    return body


# ----------------------------------------------------------------------------- vignetting
def nelder_mead(f, x0, step, iters=300, tol=1e-6):
    pts = [np.array(x0, float)] + [np.array(x0, float) + np.eye(len(x0))[i] * step for i in range(len(x0))]
    vals = [f(p) for p in pts]
    for _ in range(iters):
        order = np.argsort(vals)
        pts = [pts[i] for i in order]
        vals = [vals[i] for i in order]
        if abs(vals[-1] - vals[0]) < tol * (abs(vals[0]) + 1e-12):
            break
        centroid = np.mean(pts[:-1], axis=0)
        xr = centroid + (centroid - pts[-1])
        fr = f(xr)
        if fr < vals[0]:
            xe = centroid + 2 * (centroid - pts[-1])
            fe = f(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = centroid + 0.5 * (pts[-1] - centroid)
            fc = f(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                pts = [pts[0] + 0.5 * (p - pts[0]) for p in pts]
                vals = [f(p) for p in pts]
    i = int(np.argmin(vals))
    return pts[i], vals[i]


def vignetting(args):
    img = cv2.imread(args.vignetting, cv2.IMREAD_COLOR)
    if img is None:
        sys.exit(f"cannot read {args.vignetting}")
    info = args.info or os.path.join(os.path.dirname(os.path.abspath(args.vignetting)), "camera_info.json")
    win = window_of(load_camera(info, args.index)) if os.path.exists(info) else None
    g = img[:, :, 1].astype(np.float64)  # green channel
    h, w = g.shape
    f = max(1, int(round(max(w, h) / 320)))
    small = cv2.resize(cv2.GaussianBlur(g, (0, 0), f), (w // f, h // f), interpolation=cv2.INTER_AREA)
    ys, xs = np.mgrid[0:small.shape[0], 0:small.shape[1]]
    xs = (xs + 0.5) * f - 0.5
    ys = (ys + 0.5) * f - 0.5
    mask = (small > 8) & (small < 247)
    X, Y, Z = xs[mask], ys[mask], small[mask]
    norm = 0.5 * np.hypot(w, h)

    def residual(c, return_coef=False):
        r2 = ((X - c[0]) ** 2 + (Y - c[1]) ** 2) / norm ** 2
        A = np.stack([np.ones_like(r2), r2, r2 ** 2, r2 ** 3], 1)
        coef, *_ = np.linalg.lstsq(A, Z, rcond=None)
        e = float(np.mean((A @ coef - Z) ** 2))
        return (e, coef) if return_coef else e

    c, _ = nelder_mead(residual, [w / 2 - 0.5, h / 2 - 0.5], step=w / 20)
    e, coef = residual(c, True)
    falloff = 1 + (coef[1] + coef[2] + coef[3]) / coef[0]
    print(f"flat-field fit: centre (image px, OpenCV) = ({c[0]:.1f}, {c[1]:.1f}); RMS {np.sqrt(e):.2f} DN; "
          f"corner/centre brightness {falloff:.2f}")
    if falloff > 0.9:
        print("WARNING: almost no falloff - the sensor's lens-shading correction (LENC) is probably flattening "
              "the image, so this centre is not meaningful.")
    if win:
        X0, Y0 = image_to_sensor(c[0], c[1], win)
        print(f"optical centre estimate (sensor coords): x={X0:.1f} y={Y0:.1f}")
    else:
        print("(no camera_info.json found - pass --info to convert to sensor coordinates)")


def upload(host, body):
    import requests
    base = host if host.startswith("http") else "http://" + host
    r = requests.post(base + "/api/set_lens_calibration", json=body, timeout=60)
    print(f"upload: HTTP {r.status_code} {r.text.strip()}")
    if r.status_code == 200:
        cam = next(c for c in requests.get(base + "/api/get_camera_info", timeout=10).json()["cameras"]
                   if c.get("index", 0) == body["index"])
        w = cam["window"]
        print(f"device window now x={w['x']} y={w['y']} centerError=({w['centerError']['x']:+.2f}, "
              f"{w['centerError']['y']:+.2f}) centerRange x {w['centerRange']['xMin']}..{w['centerRange']['xMax']}"
              f" y {w['centerRange']['yMin']}..{w['centerRange']['yMax']}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", nargs="?", help="folder with frames + camera_info.json (from capture.py)")
    ap.add_argument("--board", help="inner corners COLSxROWS, e.g. 9x6")
    ap.add_argument("--square", type=float, default=1.0, help="square size (any unit; only affects extrinsics)")
    ap.add_argument("--index", type=int, default=0, help="camera index")
    ap.add_argument("--no-k3", action="store_true", help="fix k3 = 0 (use with few or centre-only views)")
    ap.add_argument("--no-tangential", action="store_true", help="fix p1 = p2 = 0")
    ap.add_argument("--upload", metavar="HOST", help="POST the result to the device")
    ap.add_argument("--vignetting", metavar="IMAGE", help="estimate the optical centre from a flat-field image")
    ap.add_argument("--info", help="camera_info.json for --vignetting (default: next to the image)")
    args = ap.parse_args()

    if args.vignetting:
        vignetting(args)
        return
    if not args.folder or not args.board:
        ap.error("folder and --board are required for checkerboard calibration")
    body = calibrate(args)
    if args.upload:
        upload(args.upload, body)


if __name__ == "__main__":
    main()
