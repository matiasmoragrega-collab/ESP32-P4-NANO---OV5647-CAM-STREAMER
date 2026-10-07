#!/usr/bin/env python3
"""Grab calibration frames from the ESP32-P4 OV5647 camera.

Frames come from http://<host>/api/capture_image?source=<n> (full-quality JPEG of the
current mode); the device's /api/get_camera_info is saved alongside as
camera_info.json so calibrate.py knows the sensor window of these images.

Examples
  python capture.py --host 192.168.137.209 --out shots            # live preview, SPACE saves, Q quits
  python capture.py --host 192.168.137.209 --out shots --board 9x6  # + live checkerboard detection
  python capture.py --host 192.168.137.209 --out shots --interval 3 --count 15   # timed
  python capture.py --host 192.168.137.209 --out shots --no-gui     # ENTER saves, q+ENTER quits
"""
import argparse
import json
import os
import sys
import time

import cv2
import numpy as np
import requests


def fetch_info(base, index):
    r = requests.get(base + "/api/get_camera_info", timeout=10)
    r.raise_for_status()
    info = r.json()
    cam = next(c for c in info["cameras"] if c.get("index", 0) == index)
    return info, cam


def fetch_frame(base, source):
    r = requests.get(f"{base}/api/capture_image?source={source}", timeout=10)
    r.raise_for_status()
    img = cv2.imdecode(np.frombuffer(r.content, np.uint8), cv2.IMREAD_COLOR)
    if img is None:
        raise RuntimeError("could not decode frame")
    return r.content, img


def parse_board(text):
    if not text:
        return None
    c, r = text.lower().split("x")
    return int(c), int(r)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="device address, e.g. 192.168.137.209")
    ap.add_argument("--out", default="shots", help="output folder")
    ap.add_argument("--source", type=int, default=0, help="camera index (default 0)")
    ap.add_argument("--count", type=int, default=0, help="stop after N saved frames (0 = until quit)")
    ap.add_argument("--interval", type=float, default=0, help="save automatically every N seconds")
    ap.add_argument("--board", help="inner corners COLSxROWS, shows live detection (e.g. 9x6)")
    ap.add_argument("--no-gui", action="store_true", help="no preview window; ENTER saves a frame")
    args = ap.parse_args()

    base = args.host if args.host.startswith("http") else "http://" + args.host
    os.makedirs(args.out, exist_ok=True)
    info, cam = fetch_info(base, args.source)
    win = cam.get("window")
    if not win:
        sys.exit("device does not report a sensor window - update the firmware first")
    with open(os.path.join(args.out, "camera_info.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1)
    print(f"mode {cam['currentResolution']['width']}x{cam['currentResolution']['height']}, window "
          f"x={win['x']} y={win['y']} {win['width']}x{win['height']} scale={win['scale']} "
          f"mirror={win['mirror']} flip={win['flip']}")
    print(f"saving to {os.path.abspath(args.out)}")

    board = parse_board(args.board)
    saved = len([n for n in os.listdir(args.out) if n.startswith("frame_") and n.endswith(".jpg")])
    start_count = saved
    last_save = 0.0

    def save(raw):
        nonlocal saved
        # Make sure the mode (and so the window) did not change under us.
        _, now = fetch_info(base, args.source)
        if now.get("window") != win or now["currentResolution"] != cam["currentResolution"]:
            sys.exit("the camera mode / window changed during capture - start a new folder")
        path = os.path.join(args.out, f"frame_{saved:03d}.jpg")
        with open(path, "wb") as f:
            f.write(raw)
        saved += 1
        print(f"saved {path}")

    while True:
        raw, img = fetch_frame(base, args.source)
        found = None
        if board:
            gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
            ok, corners = cv2.findChessboardCorners(gray, board, flags=cv2.CALIB_CB_FAST_CHECK |
                                                    cv2.CALIB_CB_ADAPTIVE_THRESH | cv2.CALIB_CB_NORMALIZE_IMAGE)
            found = ok
        if args.no_gui:
            cmd = input(f"[{saved - start_count} saved{'' if found is None else ', board ' + ('FOUND' if found else 'not found')}]"
                        " ENTER = save, q = quit: ").strip().lower()
            if cmd == "q":
                break
            raw, img = fetch_frame(base, args.source)
            save(raw)
        else:
            view = img.copy()
            if board and found:
                cv2.drawChessboardCorners(view, board, corners, True)
            label = f"saved {saved - start_count}  SPACE=save  Q=quit"
            if found is not None:
                label += "  board: " + ("FOUND" if found else "-")
            cv2.putText(view, label, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
            scale = min(1.0, 1280 / view.shape[1], 900 / view.shape[0])
            cv2.imshow("capture", cv2.resize(view, None, fx=scale, fy=scale) if scale < 1 else view)
            key = cv2.waitKey(1) & 0xFF
            if key in (ord("q"), 27):
                break
            due = args.interval > 0 and time.time() - last_save >= args.interval
            if key == ord(" ") or due:
                save(raw)
                last_save = time.time()
        if args.count and saved - start_count >= args.count:
            break

    if not args.no_gui:
        cv2.destroyAllWindows()
    print(f"{saved - start_count} frames saved in {args.out}")


if __name__ == "__main__":
    main()
