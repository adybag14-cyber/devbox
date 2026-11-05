# Native browser dialog observations

The C++23 browser gives a visible modal its own observation scope.
A late dialog can otherwise lose its controls behind the 80-control response
limit or its text behind a long document prefix. This affects agent interaction
even when the screenshot correctly shows the popup.

Observations keep `text` as the bounded document prefix used for page-status and
challenge detection. `modal_text` reports the selected modal separately, and
`viewport_text`, controls, frame metadata and scroll regions focus on that modal.
The scopes and truncation fields make this distinction explicit. The dialog
container itself is included when it scrolls. Closing the modal restores the
ordinary control view. Full retained HTML remains available through explicit
HTML capture.

Discovery examines at most 4,000 elements and 32 document/shadow roots. The
reported dialog list holds at most eight entries; three selection references
preserve a later active modal without collecting an overflow list. Hit-tested
covering modals take precedence over focus in an obscured lower native modal.
ARIA dialogs narrow the control scope only with hit-test evidence; a merely
declared, clipped or non-intercepting ARIA dialog cannot hide the page controls.
Native `:modal` elements remain fallback candidates because they block document
interaction even when no sampled point hits them.
`active_dialog` can therefore describe a modal outside the reported list, with
`dialogs_truncated` indicating the omitted entries.
The existing 80-control, 12-scroll-region, 8,000-character document/modal text
and 4,000-character viewport-text limits remain. Native and ARIA modal markers
are page evidence; they never authorize input or approve an operation. Short
dialogs count as usable content while challenge, access-denial, navigation and
loading classifications retain their precedence.

`dialog_readable` indicates observed viewport text or opaque hit-tested controls.
An empty or unreadable loading dialog remains insufficient content even when its
background document has text. Control descriptors retain DOM state for backwards
compatibility, so their mere presence does not establish readiness. Opacity checks
for readiness inspect at most 256 composed ancestors. Document challenge, error,
navigation and loading classifications retain priority.

Version 0.9.43 adds an optional `interaction_point` to each control descriptor.
The bounding rectangle remains unchanged. A clipped or partly covered button
can have an inaccessible rectangle centre while an edge remains clickable.
The browser probes the centre first, then up to eight inset edge/corner points,
using real hit testing (including open shadow roots), with at most 192 control
probes per observation. Opaque ancestry is still required. Ordinary unobscured
controls need just the original single hit test.

Prefer the reported point when present, using that observation's ID. A null
point means the bounded probe found no coordinate; it does not prove that the
control is invisible or wholly covered. `interaction_points_truncated` reports
probe-budget exhaustion. Points are recomputed on every observation and never
authorize input or establish success; verify the effect after an action.
Modal selection, control-count limits and input guards are unchanged.

Text and native control enumeration cover the selected root's light DOM.
Descendant shadow-root text and native controls can be omitted; `text_note` and
`controls_note` expose these limits and point to the screenshot for the composed
display. Locating a modal itself inside an open shadow root is supported. The
independent extension additionally enumerates controls in open shadow roots.

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

Readiness and visibility regressions at `540f868` reproduce empty-dialog
premature readiness and transparent fallback selection. The corrected
implementation at `55de69987a4a072b1fe3c6c8ad2ecda64dfe245a` passes all eight
native flows and the protocol/core suites on Windows and Linux. The bridge's
42 JavaScript checks include transparent elements, transparent shadow ancestors,
and preservation of transparent intercepting and native blockers; its ten Rust
checks also pass.

The document-status regression at `46f6641` reproduces a challenge marker in the
document being hidden by a modal-only text view. The corrected implementation at
`5e5863b2473194972cd1a4ce1655eebfcccdac88` passes eleven native browser flows on
Windows and Linux, including the document challenge, clipped ARIA scope and
unpainted-control readiness cases. Core/protocol suites pass on both platforms.
The extension's 44 JavaScript checks include exact text-budget completeness and
ordinary document context following modal text.
