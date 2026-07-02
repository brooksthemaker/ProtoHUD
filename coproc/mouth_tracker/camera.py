"""Camera sources for the demo — USB (OpenCV) or CSI (Picamera2).

Both yield RGB uint8 frames (what MediaPipe wants). Picamera2 is imported lazily
so the USB path has no hard dependency on it.
"""

import cv2
import numpy as np


class UsbCamera:
    def __init__(self, index: int, width: int, height: int):
        self.cap = cv2.VideoCapture(index)
        self.cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        self.cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        if not self.cap.isOpened():
            raise RuntimeError(f"could not open USB camera index {index}")

    def read(self):
        ok, bgr = self.cap.read()
        if not ok:
            return None
        return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)

    def close(self):
        self.cap.release()


class CsiCamera:
    def __init__(self, width: int, height: int):
        from picamera2 import Picamera2  # lazy: only needed for CSI
        self.picam2 = Picamera2()
        cfg = self.picam2.create_preview_configuration(
            main={"format": "RGB888", "size": (width, height)})
        self.picam2.configure(cfg)
        self.picam2.start()

    def read(self):
        arr = self.picam2.capture_array()
        # Picamera2 "RGB888" delivers channels in BGR order; flip to RGB.
        return np.ascontiguousarray(arr[:, :, ::-1])

    def close(self):
        self.picam2.stop()


def open_camera(source: str, width: int, height: int):
    """source: 'csi' or a USB integer index as a string."""
    if source.lower() == "csi":
        return CsiCamera(width, height)
    return UsbCamera(int(source), width, height)
