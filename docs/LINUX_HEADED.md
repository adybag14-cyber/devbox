# Headed Linux computer use

The MCP server remains native C++23. A headed Linux deployment supplies an X11
desktop (a physical display or Xvfb), a window manager, and ordinary GUI apps.
`host_computer_windows` and `host_computer_use` use native XCB/XTEST and native PNG
capture; no browser DOM, CDP endpoint, clipboard, `xdotool`, or script interpreter
is involved in these computer-use actions.

Enable `DEVBOX_COMPUTER_USE_X11=1` and set `DISPLAY` to an authorized local display
such as `:99`, with `XAUTHORITY` pointing to its private cookie file. Host runtime
and host execution must also be enabled. Linux build hosts need the XCB XTEST
headers (`libxcb-xtest0-dev` on Debian/Ubuntu, `libxcb-dev` on Alpine). Runtime
desktop installs need `libxcb.so.1` and `libxcb-xtest.so.0`. They load on demand:
headless deployments continue working without these libraries or X11 enabled.

An Xvfb display does not require a physical monitor or GPU. Use a 24-bit TrueColor
screen; 1280x800 is a practical small-VM starting point. Run the browser normally,
without `--headless`. Keep Chromium's sandbox enabled and validate its namespace
and seccomp protections in the target container. Do not expose X11 TCP, VNC, or
remote debugging ports. A virtual display is seen and controlled through MCP's
authenticated screenshots and input tools.

Windows and Linux share the same tool schemas and single-use, 180-second
observation contract. Linux binds each window to its local display, XID, process
ID, and `/proc` start time, and requires matching effective UID and PID namespace.
Actions check foreground focus, bounds, title, occlusion, and held input. A private
cross-process display lock serializes input. Cancellation releases injected keys
and buttons. Errors after input starts report an unknown outcome and require a
new observation. XCB requests have bounded waits; a display disconnect becomes a
tool error instead of Xlib's process-wide fatal handler.

Scroll coordinates select the content pane, not merely the outer window. Positive
`scroll_y` sends wheel-down notches; positive `scroll_x` sends wheel-right notches.
Check the returned image for actual movement. Input delivery does not establish
that an application accepted a form value or completed an action.

Linux capture returns lossless PNG, including when `quality` is supplied for API
compatibility. Literal Unicode typing temporarily maps an unused keycode, restores
the previous map, and never changes clipboard contents. A display with no spare
keycode rejects characters absent from its existing layout. Typing has a preflight
budget of 1024 work units: existing layout characters cost 1, and characters that
need temporary mapping cost 11. Larger requests are rejected before input; split
them into smaller calls (at most 90 unfamiliar glyphs per call). Unmapped Unicode
requires the target to support the standard `_NET_WM_PING` protocol: the backend
waits for the app's event queue before reusing/restoring a key mapping. This avoids
lost characters when the app is busy, without modifying the clipboard. Unsupported layouts/keys and windows that extend outside
the screen are rejected. Native Wayland-only sessions and Android input are out
of scope; use X11/XWayland with a matching local process identity. For container
deployments, the browser and MCP must share the UID and PID namespace.

Run `DEVBOX_MCP_TEST_BINARY=/absolute/path/devbox-mcp node
cpp-mcp/scripts/x11-computer-smoke.mjs` on a Linux host with Xvfb, a C++ compiler,
Xlib development headers, and ImageMagick. The owned-window MCP test covers pixels,
mouse/keyboard events, Unicode, scrolling, dragging, replay rejection, stale title,
occlusion and cancellation cleanup. It creates and stops only its own children.
