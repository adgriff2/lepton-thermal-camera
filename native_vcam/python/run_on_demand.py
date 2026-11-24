"""Runs lepton_cam.py unchanged. With --on-demand (used by lepton_agent.exe) it also:
  - stops cleanly, like Ctrl+C, when standard input is closed (the agent is done, or is gone)
  - exits instead of capturing for nothing when the camera feed is not available
All other options go to lepton_cam.py. This is also the entry point of the packaged lepton_cam.exe."""
import os, sys

def _stop_when_stdin_closes():
    import _thread, ctypes, signal, threading, time
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32")
    k32.GetStdHandle.restype = wintypes.HANDLE; k32.GetStdHandle.argtypes = [wintypes.DWORD]
    k32.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID, wintypes.LPVOID, wintypes.LPVOID]
    stdin = k32.GetStdHandle(0xFFFFFFF6)                                   # STD_INPUT_HANDLE
    signal.signal(signal.SIGINT, signal.default_int_handler)
    def watch():
        # polled: a blocking read on stdin can freeze DLL loading (numpy, OpenCV) in other threads
        while k32.PeekNamedPipe(stdin, None, 0, None, None, None): time.sleep(0.2)
        # let the board finish opening, so that it is also closed properly
        t = time.monotonic()
        while time.monotonic() - t < 10 and not any("_worker" in x.name for x in threading.enumerate()): time.sleep(0.05)
        time.sleep(0.3)
        _thread.interrupt_main()
    threading.Thread(target=watch, daemon=True, name="stdin-watch").start()

here = os.path.dirname(os.path.abspath(__file__))
if "--on-demand" in sys.argv:
    sys.argv.remove("--on-demand")
    sys.path.insert(0, here)
    _stop_when_stdin_closes()
    import leptonfeed
    _open = leptonfeed.try_open
    def _required(*args):
        feed = _open(*args)
        if feed is None: sys.exit(3)
        return feed
    leptonfeed.try_open = _required

if getattr(sys, "frozen", False):
    import lepton_cam                                                      # runs main()
else:
    import runpy
    target = os.path.join(os.path.dirname(os.path.dirname(here)), "lepton_cam.py")
    sys.argv[0] = target
    runpy.run_path(target, run_name="__main__")
