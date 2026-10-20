# Candidate glibc browser stack; qualify it with Chromium's sandbox enabled.
# The matching application artifact is linux-x86_64, not the legacy musl build.
FROM debian@sha256:a29215f6a35e51e22adffa17f89e9d2ef06214e64a2bad10d765c46aea49f11f
ARG CHROMIUM_VERSION=155.0.8059.79-1~deb13u1
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       ca-certificates bash nodejs npm git python3 python3-pip ripgrep curl jq gh procps util-linux unzip zip \
       xvfb xauth x11-utils openbox dbus libxcb1 libxcb-xtest0 fonts-dejavu-core fonts-noto-color-emoji \
       "chromium=${CHROMIUM_VERSION}" "chromium-common=${CHROMIUM_VERSION}" "chromium-sandbox=${CHROMIUM_VERSION}" \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 996 devbox \
    && mkdir -p /workspace \
    && useradd --uid 996 --gid 996 --no-log-init --create-home --home-dir /workspace/.home --shell /bin/bash devbox
ENV HOME=/workspace/.home NODE_ENV=production NODE_OPTIONS=--max-old-space-size=96
WORKDIR /opt/devbox
USER 996:996
CMD ["sh", "/desktop/start-headed.sh"]
