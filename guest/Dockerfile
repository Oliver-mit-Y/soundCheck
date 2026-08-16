FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libcurl4 \
        libgraphicsmagick++-q16-12 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Build this binary on the Raspberry Pi before building/running the container:
#   cd guest/rpi-rgb-led-matrix-master/utils
#   make -f Makefile.gif_api
COPY rpi-rgb-led-matrix-master/utils/gif-and-text-api /usr/local/bin/gif-and-text-api
COPY rpi-rgb-led-matrix-master/fonts /app/fonts
COPY idle-small.gif /app/idle-small.gif

RUN chmod +x /usr/local/bin/gif-and-text-api
