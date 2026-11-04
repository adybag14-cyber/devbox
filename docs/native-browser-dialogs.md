# Native browser dialog observations

The C++23 browser in 0.9.31 gives a visible modal its own observation scope.
A late dialog can otherwise lose its controls behind the 80-control response
limit or its text behind a long document prefix. This affects agent interaction
even when the screenshot correctly shows the popup.

Observations expose `active_dialog`, `dialogs`, `dialogs_truncated`, `text_scope`
and `controls_scope`. For an active modal, text, controls, frame metadata and
scroll regions describe that modal. The dialog container itself is included when
it scrolls. Closing the modal restores the ordinary document view. Full retained
HTML remains available through the existing explicit HTML capture path.

Discovery examines at most 4,000 elements, 32 document/shadow roots and eight
visible dialogs. The existing 80-control, 12-scroll-region, 8,000-character text
and 4,000-character viewport-text limits remain. Native and ARIA modal markers
are page evidence; they never authorize input or approve an operation. Short
dialogs count as usable content while challenge, access-denial, navigation and
loading classifications retain their precedence.

The real Chromium regression includes a dialog appended after 220 background
controls and a long catalogue. It exercises native and ARIA dialogs, an alert
dialog inside an open shadow root, and a dialog whose entire contents scroll.
Each case checks a screenshot, finds the close control, scrolls, verifies the
changed position through a separate observation, closes the dialog through the
normal guarded input path, and checks owned-profile cleanup.

The original implementation at `3937f58245ff9b63bf97789be85effe28b03fb21`
failed with `Visible modal close button was omitted after background controls`
on Windows and Linux. Candidate `42cd7c88b` passed all four real-browser cases
on Windows Chrome 156 and Linux Chrome 157. These are functional observations,
not a browser-wide latency or site-access success-rate claim. Hosted platform
qualification is separate from the local fixture results.
