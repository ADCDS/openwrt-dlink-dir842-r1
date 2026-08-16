# Build container for the DIR-842 R1 OpenWrt port.
#
# The ggbruno RTL8197F fork is a 2020 tree (kernel 4.14 / gcc 8.4): it needs a
# Debian 11 (bullseye)-era host, python2 among the host tools, and a non-root
# build user (OpenWrt refuses to build as root).
#
#   docker build -t owrt-dir842 .
#   docker run --rm -v "$PWD":/build -w /build owrt-dir842 ./build.sh
#
# ⚠ Keep the workdir at /build, NOT /build/openwrt: docker would create that
#   directory root-owned before anything runs and build.sh aborts with
#   "ERROR: ./openwrt already exists".
#
# If your host account is not uid 1000, build with:
#   docker build --build-arg U=$(id -u) --build-arg G=$(id -g) -t owrt-dir842 .
FROM debian:bullseye

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential clang flex bison g++ gawk gcc-multilib g++-multilib \
      gettext git libncurses5-dev libssl-dev rsync unzip zlib1g-dev file \
      wget curl subversion swig time xsltproc libelf-dev bc python3 python2 \
      python-is-python2 ca-certificates ccache quilt && \
    rm -rf /var/lib/apt/lists/*

ARG U=1000
ARG G=1000
RUN groupadd -g "$G" builder 2>/dev/null || true; \
    useradd -u "$U" -g "$G" -m -s /bin/bash builder
USER builder
ENV HOME=/home/builder FORCE_UNSAFE_CONFIGURE=1
WORKDIR /build
