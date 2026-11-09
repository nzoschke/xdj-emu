# Toolchain for the XDJ-RX3 guest pieces. Nothing Pioneer-owned is baked in:
# the image only builds this project's code and assembles a user's own
# firmware at run time (see guest/rx3/build.sh).
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git ninja-build pkg-config python3 \
        python3-venv python3-wheel python3-setuptools python3-pip libglib2.0-dev libglib2.0-dev-bin zlib1g-dev \
        busybox-static e2fsprogs libarchive-tools util-linux cpio xz-utils \
    && rm -rf /var/lib/apt/lists/*

# QEMU's configure checks this even for a linux-user-only build. Install it
# here so the actual firmware/tool build can keep --network none.
RUN python3 -m pip install --break-system-packages qemu.qmp==0.0.5
