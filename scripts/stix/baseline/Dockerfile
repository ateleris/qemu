FROM ubuntu:20.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ninja-build python3 git libglib2.0-dev libpixman-1-dev \
    && rm -rf /var/lib/apt/lists/*
