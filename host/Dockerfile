FROM python:3.14-slim-trixie

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc \
        libffi-dev \
        libjpeg62-turbo-dev \
        zlib1g-dev \
        libpq-dev \
    && rm -rf /var/lib/apt/lists/*

COPY requirenments.txt ./
RUN pip install --no-cache-dir -r requirenments.txt

COPY . .

RUN chmod +x /app/start.sh

EXPOSE 4567

CMD ["/app/start.sh"]
