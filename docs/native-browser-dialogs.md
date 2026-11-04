# Native browser dialog observations

The C++23 browser gives a visible modal its own observation scope.
A late dialog can otherwise lose its controls behind the 80-control response
limit or its text behind a long document prefix. This affects agent interaction
even when the screenshot correctly shows the popup.

Observations expose `active_dialog`, `dialogs`, `dialogs_truncated`, `text_scope`
and `controls_scope`. For an active modal, text, controls, frame metadata and
scroll regions describe that modal. The dialog container itself is included when
it scrolls. Closing the modal restores the ordinary document view. Full retained
HTML remains available through the existing explicit HTML capture path.

Discovery examines at most 4,000 elements and 32 document/shadow roots. The
reported dialog list holds at most eight entries; three selection references
preserve a later active modal without collecting an overflow list. Hit-tested
covering modals take precedence over focus in an obscured lower modal.
`active_dialog` can therefore describe a modal outside the reported list, with
`dialogs_truncated` indicating the omitted entries.
The existing 80-control, 12-scroll-region, 8,000-character text
and 4,000-character viewport-text limits remain. Native and ARIA modal markers
are page evidence; they never authorize input or approve an operation. Short
dialogs count as usable content while challenge, access-denial, navigation and
loading classifications retain their precedence.

The short-dialog exception requires nonblank text or at least one control.
An empty or whitespace-only loading dialog remains insufficient content. A
transparent ARIA dialog that does not intercept input is excluded from fallback
selection, with ancestor opacity checked over at most 256 composed ancestors.
Hit-tested blockers and native `:modal` elements remain selectable.

Text covers the selected root's light DOM. Descendant shadow-root text can be
omitted; the returned `text_note` exposes that limit and points to the screenshot
for the composed display. Locating a modal itself inside an open shadow root is
supported, as exercised by the native and extension fixtures.

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

Additional review fixtures reproduce a modal after eight visible nonmodal
dialogs and an overlapping modal whose lower sibling retains focus. Both cases
fail on `ce7fa89` and pass with the bounded selection correction at
`01a912b5803f4b3d6277398e672584ccbafe002f`. All six native browser flows and the
browser-protocol/core suites pass on Windows and Linux. The extension's real
Chrome fixtures reproduce both failures separately and pass after the matching
correction, while the reported dialog list remains capped at eight entries.
