# Software rendering on virtual browser desktops

An operator can set `DEVBOX_BROWSER_SOFTWARE_RENDERING=1` to add Chromium's
`--disable-gpu` switch to Devbox's own on-demand browser process. This is a
C++23 launch option, not an MCP argument. Its default is off, so ordinary host
browsers keep their existing rendering choice.

This option is intended for virtual displays such as Xvfb on a VM without a
GPU. The existing native desktop supervisor already starts its managed browser
with this switch; the on-demand path previously had no equivalent option.
Software rendering preserves JavaScript, HTML capture, screenshots and input.
Chromium's sandbox, Devbox's network restrictions, process ownership, session
limits and operation deadlines remain in effect.

The real headed Chromium qualification exercises rendered prices, HTML,
screenshots, scrolling and input with this option. The ordinary headless
qualification verifies that the switch is absent by default. GPU-dependent
content and WebGL performance need separate qualification for a deployment;
this option is not a claim of universal website compatibility.

Oracle investigation on 26 October found an intermittent cold CDP connection
timeout on the 1 GiB VM. A subsequent capture completed, with significant host
I/O wait and CPU steal. The software-rendering option is a candidate mitigation;
it must be measured before claiming faster or more reliable startup. No timeout
or memory ceiling is increased by the change.
