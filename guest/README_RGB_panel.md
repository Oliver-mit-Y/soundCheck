RGB panel quick test (white screen)
===================================

This folder contains `display_white.py`, a minimal script that fills an RGB matrix with white using the bundled
`rpi-rgb-led-matrix` Python bindings.

Prerequisites (on the Raspberry Pi):

- Hardware wired and powered correctly (check vendor wiring for your 64x64 panel).
- Build/install the library from `rpi-rgb-led-matrix-master` (run on the Pi):

```bash
cd guest/rpi-rgb-led-matrix-master
sudo apt update
sudo apt install -y python3-dev python3-pillow libarmadillo-dev build-essential
# follow the repo README for full build steps; typically:
make build-python
sudo make install-python
```

Running the test:

```bash
cd guest
sudo python3 display_white.py --led-rows 64 --led-cols 64 --led-chain 1
```

Notes:
- `sudo` is usually required to access GPIO pins.
- Adjust `--led-rows` / `--led-cols` / `--led-chain` to match your panel configuration.
- If the display stays black, check wiring, power supply (under-voltage causes throttling), and try `--led-gpio-mapping` options.

If you want, I can add a helper `systemd` service or a small test harness that cycles colors.
