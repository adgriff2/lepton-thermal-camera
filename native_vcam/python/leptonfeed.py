"""Writes frames to the "Lepton Thermal" virtual camera feed (protocol: ../shared/LeptonFeed.h).
Never blocks on the camera side and never raises after opening: on any error the feed just turns itself off."""
import ctypes, mmap, msvcrt, os
from ctypes import wintypes
import numpy as np, cv2

MAGIC, VERSION, FORMAT_BGRX = 0x4643564C, 1, 1
WIDTH, HEIGHT, STRIDE, SLOTS, HEADER = 640, 480, 640 * 4, 3, 4096
FRAME = STRIDE * HEIGHT
FILE_SIZE = HEADER + SLOTS * FRAME
DEFAULT_PATH = os.path.join(os.environ.get("ProgramData", r"C:\ProgramData"), "LeptonVCam", "feed", "frame.bin")

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.GetTickCount64.restype = ctypes.c_uint64
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
_k32.CloseHandle.argtypes = [wintypes.HANDLE]

def _open_exclusive_writer(path):
    """Other programs may read the file meanwhile, but a second writer gets a sharing violation."""
    GENERIC_RW, SHARE_READ, OPEN_EXISTING, ATTR_TEMPORARY = 0xC0000000, 1, 3, 0x100
    h = _k32.CreateFileW(path, GENERIC_RW, SHARE_READ, None, OPEN_EXISTING, ATTR_TEMPORARY, None)
    if h in (None, wintypes.HANDLE(-1).value):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return msvcrt.open_osfhandle(h, os.O_RDWR | os.O_BINARY)
    except Exception:
        _k32.CloseHandle(h); raise

class LeptonFeed:
    def __init__(self, path=DEFAULT_PATH):
        fd = _open_exclusive_writer(path)
        try:
            if os.fstat(fd).st_size != FILE_SIZE:
                raise OSError(f"{path}: wrong size, run install.ps1 again")
            self._mm = mmap.mmap(fd, FILE_SIZE, access=mmap.ACCESS_WRITE)
            self._fd = fd
        except Exception:
            os.close(fd); raise
        self._u32 = np.frombuffer(self._mm, np.uint32, 8, 0)
        self._u64 = np.frombuffer(self._mm, np.uint64, 2 + SLOTS, 32)      # frameCounter, tick, slotSeq[3]
        self._slots = [np.frombuffer(self._mm, np.uint8, FRAME, HEADER + i * FRAME).reshape(HEIGHT, WIDTH, 4) for i in range(SLOTS)]
        want = [MAGIC, VERSION, WIDTH, HEIGHT, STRIDE, FORMAT_BGRX, SLOTS, HEADER]
        if self._u32.tolist() != want:
            self._u32[:] = 0; self._u64[:] = 0; self._u32[:] = want
        for s in self._slots: s[:] = 0                                     # touch every page now rather than while streaming
        self._u64[2:] = 0
        self.n = int(self._u64[0]) & 0xFFFFFFFF
        self.error = None

    def send(self, bgr):
        """bgr: 480x640x3 uint8. Returns False (and stops sending) after an error."""
        if self.error: return False
        try:
            if bgr.shape != (HEIGHT, WIDTH, 3): bgr = cv2.resize(bgr, (WIDTH, HEIGHT))
            n = self.n + 1; s = (n - 1) % SLOTS
            self._u64[2 + s] = 0                                           # slot is being written
            cv2.cvtColor(bgr, cv2.COLOR_BGR2BGRA, dst=self._slots[s])      # releases the GIL
            self._u64[2 + s] = n
            self._u64[1] = _k32.GetTickCount64()
            self._u64[0] = n                                               # publish
            self.n = n
            return True
        except Exception as e:
            self.error = e; print(f"native camera feed disabled: {e}", flush=True)
            return False

    def close(self):
        try:
            self._u32 = self._u64 = self._slots = None
            self._mm.close(); os.close(self._fd)
        except Exception: pass

def try_open(path=DEFAULT_PATH):
    """LeptonFeed, or None with a message if the feed cannot be used."""
    try:
        f = LeptonFeed(path); print(f"native camera feed: {path}", flush=True); return f
    except FileNotFoundError: print(f"native camera feed not installed ({path} is missing): run native_vcam\\install.ps1 as admin", flush=True)
    except PermissionError as e:
        why = "another program is already sending frames" if getattr(e, "winerror", 0) == 32 else str(e)
        print(f"native camera feed not available: {why}", flush=True)
    except Exception as e: print(f"native camera feed not available: {e}", flush=True)
    return None
