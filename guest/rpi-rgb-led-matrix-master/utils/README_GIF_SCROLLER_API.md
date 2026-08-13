# gif_scroller_api

A high-performance GIF scroller for RGB LED matrices that fetches animated GIFs and text data from HTTP APIs, with dynamic content refresh.

## Features

- **HTTP API Integration**: Fetches GIFs and metadata from HTTP endpoints
- **JSON Parsing**: Extracts and displays configurable JSON keys
- **Dynamic Content Refresh**: Automatically detects and reloads GIFs when the `img` key changes
- **Fallback Handling**: 
  - Shows `no-signal.gif` if HTTP request fails
  - Shows `idle.gif` when JSON returns `null` for the `img` key
- **Configurable Speed**: Adjustable GIF animation speed and text scroll speed via command-line args
- **Text Formatting**: Displays JSON keys as `key1: value; key2: value; ...`
- **Continuous Looping**: Both GIF and text scroll infinitely

## Build Requirements

On Raspberry Pi OS (or similar):

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

## Building

```bash
cd guest/rpi-rgb-led-matrix-master/utils
make -f Makefile.gif_api
```

This creates the `gif_scroller_api` binary.

## Usage

```bash
sudo ./gif_scroller_api \
  --gif-url "http://192.168.1.100:5000/api/gif" \
  --json-url "http://192.168.1.100:5000/api/status" \
  --keys "song,artist,album" \
  --gif-speed 2 \
  --text-speed 2 \
  --gif-multiplier 1.0 \
  --api-interval 5000
```

### Arguments

- `--gif-url <url>` (required): HTTP endpoint that returns a GIF file
- `--json-url <url>` (required): HTTP endpoint that returns JSON with metadata
- `--keys <key1,key2,...>` (required): Comma-separated JSON keys to display
- `--gif-speed <int>`: GIF scroll speed in pixels per frame (default: 2)
- `--text-speed <int>`: Text scroll speed in pixels per frame (default: 2)
- `--gif-multiplier <float>`: GIF animation speed multiplier; 1.0 = normal, 2.0 = 2× faster (default: 1.0)
- `--api-interval <ms>`: Poll interval for JSON/GIF updates in milliseconds (default: 5000)

## API Requirements

### GIF Endpoint

Should return raw GIF data (binary). Example:

```bash
curl http://192.168.1.100:5000/api/gif > test.gif
```

### JSON Endpoint

Should return JSON with at least an `img` field (unique identifier for the current GIF):

```json
{
  "img": "song_123_abc.gif",
  "song": "Bohemian Rhapsody",
  "artist": "Queen",
  "album": "A Night at the Opera",
  "year": 1975
}
```

**Fallback Behavior:**

- When `img` is `null` or missing → displays `idle.gif`:
```json
{
  "img": null,
  "song": "N/A"
}
```

- When HTTP request fails (timeout, network error, etc.) → displays `no-signal.gif`

- When GIF download fails → displays `no-signal.gif`

## Example with Flask Backend

```python
from flask import Flask, send_file
import requests

app = Flask(__name__)

CURRENT_GIF = "animation.gif"
METADATA = {
    "img": "animation_v1.gif",
    "song": "Track Name",
    "artist": "Artist Name",
    "album": "Album Name"
}

@app.route('/api/gif', methods=['GET'])
def get_gif():
    return send_file(CURRENT_GIF, mimetype='image/gif')

@app.route('/api/status', methods=['GET'])
def get_status():
    return METADATA

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
```

## Performance Notes

- Frames are pre-cached in memory for smooth playback
- Double-buffering via `SwapOnVSync()` minimizes flicker
- API polling runs in a background thread (non-blocking)
- Content updates happen immediately when GIF changes (frame counter resets)
- ~60 FPS max refresh rate

## Troubleshooting

### Fallback Image Display

- **Seeing `no-signal.gif` (red X on black)**
  - HTTP request failed → Check network connectivity and API URL
  - GIF download failed → Verify GIF URL and file format
  
- **Seeing `idle.gif`**
  - JSON returned `null` for `img` key → This is expected idle/standby state
  - Your backend intentionally paused content

### General Troubleshooting

- **"API JSON fetch failed"**: Check the JSON URL and network connectivity
- **"img: null, showing no-signal"**: Your API returned `null` for the `img` key (expected behavior)
- **Text or GIF not scrolling**: Adjust `--gif-speed` and `--text-speed` values
- **Matrix display issues**: Try `--led-gpio-mapping adafruit-hat` or adjust `--led-slowdown-gpio`

## Customizing Fallback Images

By default, the program generates placeholder images at runtime:
- `/tmp/no_signal.gif` — Red "X" on black (HTTP failures)
- `/tmp/idle.gif` — Red "X" on black (JSON returns null)

To use custom images:

```bash
# Replace idle image with your own
cp /path/to/your/idle.gif /tmp/idle.gif

# Replace no-signal image with your own
cp /path/to/your/no_signal.gif /tmp/no_signal.gif

# Then restart the program
sudo systemctl restart gif-scroller.service
```

**Image Requirements:**
- Format: GIF (animated or static)
- Dimensions: 64×48 pixels (if using 64×64 matrix with 16-pixel text bar)
- Use only if you want custom fallback appearance; dynamic generation works fine otherwise

## Systemd Service (Optional)

Create `/etc/systemd/system/gif-scroller.service`:

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
  --gif-url "http://127.0.0.1:5000/api/gif" \
  --json-url "http://127.0.0.1:5000/api/status" \
  --keys "song,artist" \
  --api-interval 5000
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
```

Enable and start:

```bash
sudo systemctl daemon-reload
sudo systemctl enable gif-scroller.service
sudo systemctl start gif-scroller.service
sudo systemctl status gif-scroller.service
```

View logs:

```bash
sudo journalctl -u gif-scroller.service -f
```
