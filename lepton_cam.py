"""Live FLIR Lepton viewer (+ optional virtual webcam) for the FT2232H dev kit. Supports Lepton 3 (160x120) and Lepton 1/2 (80x60).
  python lepton_cam.py --model 3            preview window (q quit, p palette, f flip, m mirror)
  python lepton_cam.py --model 3 --vcam     also publish as a webcam (needs OBS Virtual Camera)
  python lepton_cam.py --model 3 --nativecam   also publish as the Windows camera "FLIR Lepton" (see native_vcam/README.md)
Channel A = SPI video (CS on AD3). A reader thread only reads USB; a worker thread parses/CRC-checks, so no segments are missed."""
import sys, os, time, argparse, threading, queue
try:
    import ftd2xx as ft, numpy as np, cv2
except ImportError:
    sys.exit("run: pip install ftd2xx numpy opencv-python pyvirtualcam")

PKT, NPKT, CS = 164, 60, 1 << 3
OUTW, OUTH = 640, 480
PALETTES = [("gray", None), ("inferno", cv2.COLORMAP_INFERNO), ("jet", cv2.COLORMAP_JET), ("hot", cv2.COLORMAP_HOT)]

def _mk():
    t = []
    for i in range(256):
        c = i << 8
        for _ in range(8): c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        t.append(c)
    return np.array(t, dtype=np.uint32)
CRC_T = _mk()

def crc_ok_all(P):
    """P: (60,164) uint8 array. Vectorised CRC-16/CCITT check of every packet."""
    P = P.copy(); P[:, 0] &= 0x0F
    rx = (P[:, 2].astype(np.uint32) << 8) | P[:, 3]; P[:, 2:4] = 0
    c = np.zeros(len(P), dtype=np.uint32)
    for j in range(PKT):
        c = ((c << 8) & 0xFFFF) ^ CRC_T[((c >> 8) ^ P[:, j]) & 0xFF]
    return bool((c == rx).all())

class Grabber:
    def __init__(self, model="auto", div=1, chunks=3, check_crc=True):
        self.model, self.n, self.check_crc, self.div = model, PKT * NPKT * chunks, check_crc, div
        self.lock = threading.Lock(); self.frame = None; self.count = 0; self.stop = False; self.err = None
        self.hist = {}; self.crc_bad = 0; self.runs = 0; self.resyncs = 0; self.dropped = 0
        self.canvas = np.zeros((120, 160), np.uint16); self.mask = 0
        self.q = queue.Queue(maxsize=400); self.last_ok = time.time()
        self.h = h = ft.open(0)
        h.resetDevice(); h.purge(3); h.setTimeouts(2000, 2000); h.setLatencyTimer(1); h.setUSBParameters(65536, 65536)
        h.setBitMode(0, 0); h.setBitMode(0, 2)
        h.write(bytes([0x85, 0x8A, 0x97, 0x8D, 0x86, div & 0xFF, div >> 8])); time.sleep(0.05); h.purge(3)
        self.tr = threading.Thread(target=self._reader, daemon=True); self.tw = threading.Thread(target=self._worker, daemon=True)
    def start(self): self.tr.start(); self.tw.start()
    def _cmd(self):                                      # clock n bytes in; no CS change, so the stream has no gaps
        return bytes([0x20, (self.n-1) & 0xFF, (self.n-1) >> 8, 0x87])
    def _start_stream(self):
        h = self.h; d = 0x03 | CS
        h.setBitMode(0, 0); h.setBitMode(0, 2)           # abort anything still queued in the MPSSE
        h.write(bytes([0x85, 0x8A, 0x97, 0x8D, 0x86, self.div & 0xFF, self.div >> 8, 0x80, 0x01 | CS, d]))
        time.sleep(0.3); h.purge(3)                      # CS high > 185 ms so the Lepton restarts its VoSPI state
        while not self.q.empty():
            try: self.q.get_nowait()
            except queue.Empty: break
        h.write(bytes([0x80, 0x01, d]) + self._cmd() * 2)   # CS low, keep two reads queued
    def _read_n(self):
        out = b""
        while len(out) < self.n:
            r = self.h.read(self.n - len(out))
            if not r: raise IOError("SPI read timeout")
            out += r
        return out
    def _reader(self):                                   # does nothing but read, so the clock never stops
        try:
            self._start_stream()
            while not self.stop:
                buf = self._read_n()
                self.h.write(self._cmd())                # always one read queued ahead of the one being read
                try: self.q.put_nowait(buf)
                except queue.Full: self.dropped += 1
                if time.time() - self.last_ok > 3: self._start_stream(); self.resyncs += 1; self.last_ok = time.time()
        except Exception as e:
            self.err = e
        finally:
            try: self.h.setBitMode(0, 0); self.h.write(bytes([0x80, 0x01, 0x00])); self.h.close()
            except Exception: pass
    def _worker(self):
        rows = [None] * NPKT; expected = 0
        try:
            while not self.stop:
                try: buf = self.q.get(timeout=0.2)
                except queue.Empty: continue
                arr = np.frombuffer(buf, dtype=np.uint8).reshape(-1, PKT)
                idx = np.nonzero((arr[:, 0] & 0x0F) != 0x0F)[0]        # drop discard packets
                nums = arr[idx, 1].tolist()
                for i, n in zip(idx.tolist(), nums):
                    if n == 0: rows[0] = arr[i].copy(); expected = 1
                    elif n == expected and n < NPKT: rows[n] = arr[i].copy(); expected += 1
                    else: expected = 0
                    if expected != NPKT: continue
                    expected = 0; self.runs += 1
                    ttt = (int(rows[20][0]) >> 4) & 7; self.hist[ttt] = self.hist.get(ttt, 0) + 1
                    if self.model == "auto" and 1 <= ttt <= 4: self.model = "3"
                    if self.model == "auto" and self.runs >= 60: self.model = "2"
                    if (self.model == "3" and not 1 <= ttt <= 4) or self.model == "auto": continue   # invalid/duplicate segment
                    P = np.stack(rows)
                    if self.check_crc and not crc_ok_all(P): self.crc_bad += 1; continue
                    px = np.ascontiguousarray(P[:, 4:]).view(">u2").astype(np.uint16).reshape(-1)
                    if self.model == "2":
                        with self.lock: self.frame = px.reshape(60, 80); self.count += 1
                    else:
                        self.canvas[(ttt-1)*30:ttt*30] = px.reshape(30, 160)
                        self.mask = 1 if ttt == 1 else self.mask | (1 << (ttt-1))
                        if ttt == 4 and self.mask == 0b1111:
                            with self.lock: self.frame = self.canvas.copy(); self.count += 1
                    self.last_ok = time.time()
        except Exception as e:
            self.err = e

def render(px, pal, flip, mirror, lo_hi):
    a = px.astype(np.float32)
    lo, hi = np.percentile(a, 1), np.percentile(a, 99)
    hi = max(hi, lo + 20)
    old = lo_hi[0]
    lo_hi[0] = (lo, hi) if old is None else (0.9*old[0] + 0.1*lo, 0.9*old[1] + 0.1*hi)
    lo, hi = lo_hi[0]
    g = np.clip((a - lo) / (hi - lo) * 255, 0, 255).astype(np.uint8)
    img = cv2.resize(g, (OUTW, OUTH), interpolation=cv2.INTER_CUBIC)
    img = cv2.applyColorMap(img, pal[1]) if pal[1] is not None else cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)
    if flip: img = cv2.flip(img, 0)
    if mirror: img = cv2.flip(img, 1)
    return img

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vcam", action="store_true"); ap.add_argument("--no-preview", action="store_true")
    ap.add_argument("--model", choices=["auto", "2", "3"], default="auto")
    ap.add_argument("--div", type=int, default=2, help="SPI clock = 60 MHz/((1+div)*2): 2=10 MHz (safe), 1=15 MHz")
    ap.add_argument("--chunks", type=int, default=3, help="60-packet blocks per USB read")
    ap.add_argument("--no-crc", action="store_true")
    ap.add_argument("--nativecam", action="store_true", help="publish to the native Windows camera (native_vcam)")
    a = ap.parse_args()
    feed, fedc = None, 0
    if a.nativecam:                                      # before the Grabber starts: opening touches a few MB of memory
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "native_vcam", "python"))
        import leptonfeed; feed = leptonfeed.try_open()
    g = Grabber(a.model, a.div, a.chunks, not a.no_crc); g.start()
    cam = None
    if a.vcam:
        import pyvirtualcam
        cam = pyvirtualcam.Camera(width=OUTW, height=OUTH, fps=30, fmt=pyvirtualcam.PixelFormat.BGR)
        print(f"virtual camera: {cam.device}")
    pi, flip, mirror, lo_hi = 1, False, False, [None]; t0 = lastp = time.time(); lastc = 0
    try:
        while True:
            if g.err: raise g.err
            with g.lock: px = None if g.frame is None else g.frame.copy(); count = g.count
            if time.time() - lastp > 2:
                now = time.time()
                print(f"model={g.model} frames={count} fps={(count-lastc)/(now-lastp):.1f} runs={g.runs} seg-ids={dict(sorted(g.hist.items()))} crc_bad={g.crc_bad} resyncs={g.resyncs} dropped={g.dropped} queue={g.q.qsize()}   ", flush=True)
                lastp, lastc = now, count
            if px is None: time.sleep(0.02); continue
            img = render(px, PALETTES[pi], flip, mirror, lo_hi)
            if feed and count != fedc: feed.send(img); fedc = count     # memory copy only, never waits for the camera
            if cam: cam.send(img); cam.sleep_until_next_frame()
            if not a.no_preview:
                cv2.imshow("Lepton", img); k = cv2.waitKey(1 if cam else 15) & 0xFF
                if k == ord("q"): break
                elif k == ord("p"): pi = (pi + 1) % len(PALETTES)
                elif k == ord("f"): flip = not flip
                elif k == ord("m"): mirror = not mirror
                if cv2.getWindowProperty("Lepton", cv2.WND_PROP_VISIBLE) < 1: break
            elif not cam: time.sleep(0.03)
    except KeyboardInterrupt: pass
    finally:
        g.stop = True; g.tr.join(timeout=3)
        if cam: cam.close()
        if feed: feed.close()
        cv2.destroyAllWindows()
    print()

main()
