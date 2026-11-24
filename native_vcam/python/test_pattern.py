"""Sends a moving test pattern to the "Lepton Thermal" camera.
  python test_pattern.py [--seconds 30] [--fps 9] [--feed PATH]
Colour bars with a white bar sweeping left to right and a frame number, so frozen, torn or colour-swapped video is obvious."""
import argparse, sys, time
import numpy as np, cv2
import leptonfeed

ap = argparse.ArgumentParser()
ap.add_argument("--seconds", type=float, default=30); ap.add_argument("--fps", type=float, default=9)
ap.add_argument("--feed", default=leptonfeed.DEFAULT_PATH)
a = ap.parse_args()
feed = leptonfeed.try_open(a.feed)
if not feed: sys.exit(1)

W, H = leptonfeed.WIDTH, leptonfeed.HEIGHT
bars = [("RED", (0, 0, 255)), ("GREEN", (0, 255, 0)), ("BLUE", (255, 0, 0)), ("GRAY", (128, 128, 128))]
base = np.zeros((H, W, 3), np.uint8)
for i, (name, c) in enumerate(bars):
    base[:, i * W // 4:(i + 1) * W // 4] = c
    cv2.putText(base, name, (i * W // 4 + 20, 60), cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 255), 2)
t0 = time.perf_counter(); n = 0; worst = total = 0.0
try:
    while time.perf_counter() - t0 < a.seconds:
        img = base.copy(); x = (n * 12) % W
        img[100:400, x:x + 24] = 255
        cv2.putText(img, f"frame {n}", (180, 450), cv2.FONT_HERSHEY_SIMPLEX, 1.4, (0, 0, 0), 8)
        cv2.putText(img, f"frame {n}", (180, 450), cv2.FONT_HERSHEY_SIMPLEX, 1.4, (255, 255, 255), 2)
        t = time.perf_counter()
        if not feed.send(img): sys.exit(1)
        t = time.perf_counter() - t; worst = max(worst, t); total += t
        n += 1
        time.sleep(max(0, t0 + n / a.fps - time.perf_counter()))
except KeyboardInterrupt: pass
el = time.perf_counter() - t0; feed.close()
print(f"OK sent={n} fps={n / el:.2f} send_ms avg={total / max(n, 1) * 1000:.3f} worst={worst * 1000:.3f}")
