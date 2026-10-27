# Native desktop progress and failure diagnostics

The Linux window manager handles a window event with several X11 requests. On
a resource-constrained host, those replies can arrive successfully while the
whole map or focus operation takes more than five seconds. Previously only the
outer event loop refreshed the progress timestamp. The desktop supervisor could
therefore restart a working private display and interrupt a browser CDP request.

Each received X11 reply now refreshes the progress timestamp. Waiting for a
reply does not refresh it. The two-second individual reply deadline, five-second
progress threshold, ownership checks and cancellation behavior are unchanged.

The desktop status also retains `last_desktop_exit`, containing a bounded error
and observation time. Window-manager failures include the actual worker error
or distinguish missing readiness from a stalled progress timestamp. Successful
recovery no longer erases the previous failure's diagnostic reason.

The regression uses a single-client relay in front of an owned Xvfb fixture:

- Replies delayed by 700 milliseconds keep making progress across an operation
  lasting more than five seconds. The old implementation fails the health check;
  the repaired implementation stays healthy and maps the window.
- Withholding a requested reply for five seconds still produces
  `DESKTOP_WINDOW_MANAGER_REPLY_DEADLINE` within the unchanged per-reply bound.
- Existing independent focus, stacking, fullscreen, restore, close, ownership,
  SIGPIPE and cancellation checks remain, including file-descriptor accounting.

The relay is test-only C++23 code. It adds no runtime library, executable, shell,
environment switch or production delay. A production CDP EOF remains a symptom
with several possible causes; the retained desktop error and a fresh end-to-end
browser check are needed to attribute a particular occurrence.
