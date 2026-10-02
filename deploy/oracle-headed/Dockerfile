FROM alpine@sha256:85fe1e81d6758c208f3e1eed4338a1997e19d4be002d4dd32d3100c9a8c010a0
RUN apk add --no-cache libstdc++ libgcc ca-certificates bash nodejs npm git python3 py3-pip ripgrep curl jq github-cli procps util-linux unzip zip \
    xvfb xauth xset xdpyinfo openbox chromium font-dejavu font-noto-emoji libxcb imagemagick xdotool xwininfo dbus \
    && addgroup -g 996 devbox \
    && mkdir -p /workspace \
    && adduser -D -u 996 -G devbox -h /workspace/.home devbox
ENV HOME=/workspace/.home NODE_ENV=production NODE_OPTIONS=--max-old-space-size=96
WORKDIR /opt/devbox
USER 996:996
CMD ["sh", "/desktop/start-headed.sh"]
