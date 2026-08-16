# GIF + JSON Text API Scroller

`gif-and-text-api` shows a GIF/image from an API across the full RGB matrix and overlays a small scrolling text bar at the bottom. The GIF is decoded into RAM only when the JSON response changes, so normal playback does not wait on network or image decoding.

## Build

Install the native dependencies on the Raspberry Pi:

```sh
sudo apt-get install libgraphicsmagick++-dev libcurl4-openssl-dev
```

Build from the utils directory:

```sh
cd guest/rpi-rgb-led-matrix-master/utils
make -f Makefile.gif_api
```

## Run

```sh
sudo ./gif-and-text-api \
  --led-rows=32 --led-cols=64 --led-chain=1 --led-slowdown-gpio=4 \
  --json-url=http://host:4567/api/info \
  --gif-url=http://host:4567/api/art/cover.gif \
  --keys=song,artist,album,year \
  --font=../fonts/5x7.bdf \
  --text-bar-height=8 \
  --scroll-speed=7 \
  --gif-speed=-1
```

All standard `rpi-rgb-led-matrix` flags such as `--led-rows`, `--led-cols`, `--led-chain`, `--led-parallel`, `--led-brightness`, `--led-hardware-mapping`, `--led-gpio-mapping`, and `--led-slowdown-gpio` can be passed through unchanged.

## Options

- `--keys=key1,key2,key3`: JSON keys to display as `key:value` pairs.
- `--json-url=URL`: API endpoint returning a flat JSON object or `null`, for example `/api/info`.
- `--gif-url=URL`: API endpoint returning the current GIF/image bytes, for example `/api/art/cover.gif`.
- `--font=PATH`: BDF font for the scrolling text.
- `--text-bar-height=N`: bottom bar height in pixels. It is automatically raised to at least the font height.
- `--scroll-speed=N`: approximate letters per second. Positive scrolls right to left, negative left to right.
- `--gif-speed=N`: override GIF frame delay in milliseconds. Use `-1` to keep the GIF timing.
- `--idle-image=PATH`: optional local GIF/PNG/JPG shown when the JSON endpoint returns exactly `null`.
- `--poll-ms=N`: JSON polling interval. Default is `1000`.
- `--text-color=r,g,b`: text color. Default is white.
- `--bar-color=r,g,b`: text bar background. Default is black.
- `--letter-spacing=N`: extra pixel spacing between letters.

## Behavior

When the JSON returned by the API changes, the program immediately builds the new scroll text and downloads/decode the GIF again. The URL does not need to end in `.json`; it only needs to return JSON. The previously loaded animation keeps playing while the new one is being fetched and decoded. If the JSON endpoint returns `null`, the text is hidden and `--idle-image` is shown if provided.

If fetching or decoding fails, the bottom text changes to `error` and the last successfully loaded animation remains visible.

## Debug Output

The program prints trace messages to stderr while it runs:

- `[config]`: parsed URLs, keys, font and timing settings.
- `[poll]`: JSON polling, detected changes and idle mode.
- `[fetch]`: successful HTTP requests and response sizes.
- `[json]`: extracted text or missing keys.
- `[image]`: decoded GIF/image frame counts.
- `[state]`: when a new idle or active state is applied.

If the panel only shows `error`, run it from a terminal and look for the first failed step after `[poll] fetching JSON`.
