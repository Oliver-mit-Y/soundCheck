# gif_scroller_api

API-driven GIF display for a 64x64 RGB LED matrix.

The program polls a JSON metadata endpoint and downloads a GIF endpoint whenever the JSON payload changes. The GIF is scaled to fill the whole matrix. A black text bar is drawn over the bottom of the GIF, and configured JSON fields scroll across that bar.

## Build Requirements

On Raspberry Pi OS:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  python3-dev \
  libgraphicsmagick++-dev \
  libcurl4-openssl-dev \
  nlohmann-json3-dev \
  pkg-config
```

## Build

```bash
cd guest/rpi-rgb-led-matrix-master/utils
make -f Makefile.gif_api
```

This creates `gif_scroller_api`.

## Usage

```bash
sudo ./gif_scroller_api \
  --gif-url "http://192.168.1.100:4567/api/art/cover.gif" \
  --json-url "http://192.168.1.100:4567/api/info" \
  --keys "song_name,artist,album,year" \
  --led-gpio-mapping adafruit-hat-pwm \
  --led-rows 64 \
  --led-cols 64 \
  --led-chain 1 \
  --led-slowdown-gpio 2 \
  --text-speed 2 \
  --gif-multiplier 1.0 \
  --api-interval 5000 \
  --idle-path "/home/pi/idle.gif"
```

Arguments:

- `--gif-url <url>`: HTTP endpoint returning GIF bytes, for example `/api/art/cover.gif`.
- `--json-url <url>`: HTTP endpoint returning metadata JSON, for example `/api/info`.
- `--keys <key1,key2,...>`: JSON fields to show in the scrolling text.
- `--text-speed <int>`: Text scroll speed in pixels per frame. Default: `2`.
- `--gif-multiplier <float>`: GIF animation speed multiplier. `1.0` is original speed, `2.0` is twice as fast. Default: `1.0`.
- `--api-interval <ms>`: Metadata polling interval. Default: `5000`.
- `--font-path <path>`: BDF font path. Default: `../fonts/7x13.bdf` when running from `utils`.
- `--bar-height <px>`: Bottom text bar height. Default: `16`.
- `--idle-path <path>`: Optional local image/GIF to show when the JSON endpoint returns `null`.
- `--gif-speed <int>`: Accepted for compatibility, but not used. The GIF fills the canvas and does not spatially scroll.
- `--led-gpio-mapping`, `--led-rows`, `--led-cols`, `--led-chain`, and other `--led-*` flags are passed through to the RGB matrix library. Use the same values that worked with the examples.

## API Contract

The GIF endpoint should return raw image bytes:

```bash
curl http://192.168.1.100:4567/api/art/cover.gif > test.gif
```

The JSON endpoint should return an object with an `img` field when something is playing:

```json
{
  "img": "https://i.scdn.co/image/...",
  "song_name": "Bohemian Rhapsody",
  "artist": "Queen",
  "album": "A Night at the Opera",
  "year": "1975"
}
```

The full JSON payload is used as the change token. Any metadata change causes the Pi to download the GIF again and reset animation playback.

When nothing is playing, return:

```json
null
```

In that state, the matrix is cleared. If `--idle-path` is set, that local image/GIF is shown instead.

If the API request, GIF download, GIF ping, or frame decoding fails, the matrix displays `error` text.

## Performance Notes

- The program pings image metadata first, then decodes only the frame it needs.
- It does not load all GIF frames into memory at startup.
- Rendering uses `SwapOnVSync()` double-buffering.
- The text bar intentionally overlays the bottom of the GIF.

## Systemd Example

```ini
[Unit]
Description=LED GIF Scroller (API-driven)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/home/pi/soundCheck/guest/rpi-rgb-led-matrix-master/utils
ExecStart=/home/pi/soundCheck/guest/rpi-rgb-led-matrix-master/utils/gif_scroller_api \
  --gif-url "http://192.168.1.100:4567/api/art/cover.gif" \
  --json-url "http://192.168.1.100:4567/api/info" \
  --keys "song_name,artist,album,year" \
  --led-gpio-mapping adafruit-hat-pwm \
  --led-rows 64 \
  --led-cols 64 \
  --led-chain 1 \
  --led-slowdown-gpio 2 \
  --api-interval 5000
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```
