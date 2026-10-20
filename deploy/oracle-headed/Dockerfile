# Alpine 3.24: qualified separately from the core's older musl ABI baseline.
FROM alpine@sha256:294b683cb724975bec92580e1e685676bd4b50bda910ddb8c51d4cabeaec77e6
RUN apk add --no-cache libstdc++ libgcc ca-certificates bash nodejs npm git python3 py3-pip ripgrep curl jq github-cli procps util-linux unzip zip \
    xvfb xauth xdpyinfo openbox chromium font-dejavu font-noto-emoji libxcb dbus \
    && addgroup -g 996 devbox \
    && mkdir -p /workspace \
    && adduser -D -u 996 -G devbox -h /workspace/.home devbox
ENV HOME=/workspace/.home NODE_ENV=production NODE_OPTIONS=--max-old-space-size=96
WORKDIR /opt/devbox
USER 996:996
CMD ["sh", "/desktop/start-headed.sh"]
