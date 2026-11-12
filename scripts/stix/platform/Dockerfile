# SPDX-License-Identifier: GPL-2.0-or-later
#
# The STIX platform controller image (platform plan PL-7; size budget
# D-PLu). Build from the repo root (needs network: the meson wraps
# download during configure):
#
#   docker build -f scripts/stix/platform/Dockerfile -t stix-platform .
#
# Run it only through scripts/stix/platform/run-container.sh: the API
# (8080/tcp) and the GS TC port (21502/udp) are published on 127.0.0.1
# only (D-PLj). The final stage carries only qemu-system-sparc, its
# runtime libraries, python3 and the controller; no make install (it
# would add pc-bios firmware, keymaps and the other tools), no
# python3-full, --no-install-recommends everywhere.

# Both stages are pinned to one ubuntu:24.04 digest (pinned 2026-11-12;
# to move it: docker pull ubuntu:24.04, then the RepoDigests entry). One
# ARG pins both, so the layer is stored once (D-PLu).
ARG UBUNTU_DIGEST=2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5

# hadolint ignore=DL3008
FROM ubuntu:24.04@sha256:${UBUNTU_DIGEST} AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build python3 python3-venv pkg-config \
        libglib2.0-dev libpixman-1-dev flex bison git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . /src
# No libcurl-dev and no libssh-dev here, so the binary does not link
# them (D-PLu; the host build links both). ca-certificates is needed by
# the wrap download: git only Recommends it, and recommends are off.
# WORKDIR /src/build makes the build dir: build*/ is dockerignored, so
# the COPY did not bring one.
WORKDIR /src/build
RUN ../configure --target-list=sparc-softmmu --disable-docs \
        --disable-install-blobs --enable-werror \
    && make -j"$(nproc)" qemu-system-sparc \
    && strip qemu-system-sparc

# hadolint ignore=DL3008
FROM ubuntu:24.04@sha256:${UBUNTU_DIGEST}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3-minimal libpython3.12-stdlib libglib2.0-0t64 \
        libpixman-1-0 zlib1g tini \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --user-group --home-dir /var/lib/stix stix \
    && mkdir -p /var/lib/stix /config /elf /scenarios /idb \
    && chown stix:stix /var/lib/stix
# The flash and run data live on the volume mounted at /var/lib/stix; a
# new named volume takes its ownership from this image directory.
# The probe must sit at /opt/stix/tmtc-probe.py: stixplat/probe_lib.py
# resolves <the stixplat package>/../../tmtc-probe.py, and the package is
# /opt/stix/platform/stixplat (as scripts/stix/platform/stixplat/../..
# is scripts/stix/ in the repo; D-PLi).
COPY --from=build /src/build/qemu-system-sparc /opt/qemu/bin/
COPY scripts/stix/tmtc-probe.py /opt/stix/tmtc-probe.py
COPY scripts/stix/platform/ /opt/stix/platform/
USER stix
WORKDIR /var/lib/stix
# tini: PID 1, so QEMU is reaped and SIGTERM reaches the controller.
ENTRYPOINT ["tini", "--", "python3", "/opt/stix/platform/stixplat.py"]
# /config is a read-only mount. Another sample:
#   scripts/stix/platform/run-container.sh --config run-gates.toml
CMD ["--config", "/config/platform.toml"]
