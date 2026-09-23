# Structured WSL backend

`devbox_wsl` is a C++23 trusted-operator tool on Windows. It has three actions: `distributions`, `map_path`, and `run`. Other platforms explicitly report unsupported, while disabled host execution reports permission denied and a missing Windows WSL bridge reports unavailable.

Run requests name a registered distribution, an absolute Linux executable and an absolute Linux working directory. Arguments are passed literally through `wsl.exe --exec`; there is no host-shell command field. The guest receives a clean environment with a fixed PATH and UTF-8 locale. The distribution's configured default user must be non-root. Path mapping uses that distribution's `wslpath`, rather than guessing drive mount locations.

A guest GNU `timeout` applies the requested 1–300 second deadline with a one-second kill-after interval. The outer bridge also has a bounded deadline and captured output. This is not an autonomous sandbox, nor a proof that an arbitrary trusted program cannot create descendants outside its process group. A Windows bridge cancellation is reported as `cancel_requested` with unverified guest termination; it is not relabelled as successful workload termination. The backend issues no distribution install, unregister, shutdown or terminate command.

Required guest utilities are `/usr/bin/env`, `/usr/bin/timeout`, and `/usr/bin/wslpath`. Registration/distribution selection is explicit; the tool does not change the default distribution. Legacy host tools remain available.

Local qualification used the existing Ubuntu distribution: literal Unicode/metacharacter argv, stdin roundtrip, Windows-to-Linux mapping, non-root default-user policy and guest timeout passed. Linux builds verified the unsupported-platform contract. This does not certify physical Android/mobile lifecycle behavior or broaden native desktop-input support beyond its existing Windows implementation.
