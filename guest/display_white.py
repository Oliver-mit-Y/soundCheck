#!/usr/bin/env python3
"""Display a plain white screen on a 64x64 RGB LED matrix using rpi-rgb-led-matrix bindings.

Place this file in the `guest` folder alongside the `rpi-rgb-led-matrix-master` checkout.
Run on the Raspberry Pi with hardware connected and the library built/installed.
"""
import os
import sys
import time

# Ensure the bindings package is importable from the bundled repo
here = os.path.abspath(os.path.dirname(__file__))
bindings_path = os.path.join(here, 'rpi-rgb-led-matrix-master', 'bindings', 'python')
if bindings_path not in sys.path:
    sys.path.insert(0, bindings_path)
# Also ensure the bundled `samples` folder is importable (some setups lack __init__.py)
samples_path = os.path.join(bindings_path, 'samples')
if samples_path not in sys.path:
    sys.path.insert(0, samples_path)

# Try a few import strategies to locate SampleBase
SampleBase = None
import importlib
try:
    from samples.samplebase import SampleBase  # preferred when samples is a package
except Exception:
    try:
        # fallback: import as top-level module if samples_path is on sys.path
        from samplebase import SampleBase
    except Exception:
        # last resort: load module directly from file
        loader_path = os.path.join(samples_path, 'samplebase.py')
        if os.path.exists(loader_path):
            try:
                spec = importlib.util.spec_from_file_location('samplebase', loader_path)
                module = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(module)
                SampleBase = getattr(module, 'SampleBase')
            except Exception as exc:
                print('Failed to load samplebase.py directly:', exc)
        else:
            print('Could not find samplebase.py in', samples_path)

if SampleBase is None:
    print('Error: could not import SampleBase from the rpi-rgb-led-matrix samples.')
    print('Make sure you built/installed the Python bindings and that', bindings_path, 'is reachable.')
    sys.exit(2)


class WhiteScreen(SampleBase):
    def __init__(self, *args, **kwargs):
        super(WhiteScreen, self).__init__(*args, **kwargs)

    def run(self):
        canvas = self.matrix.CreateFrameCanvas()

        # Fill the whole canvas white
        for y in range(canvas.height):
            for x in range(canvas.width):
                canvas.SetPixel(x, y, 255, 255, 255)

        canvas = self.matrix.SwapOnVSync(canvas)

        try:
            # Keep the white screen up until interrupted
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            self.matrix.Clear()


if __name__ == '__main__':
    app = WhiteScreen()
    app.process()
