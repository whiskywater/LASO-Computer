# Public upstream comparison

Audited read-only on 2026-09-29.

## Source state

The public [`whiskywater/LASO-Computer`](https://github.com/whiskywater/LASO-Computer) default branch is `main` at `66af16c66df1c0b47c1b13d96db049db258901e6`. It is Apache-2.0 licensed and currently contains its license and a short project description, not an executable implementation. It has no tags or releases.

Public PR #1, [`Rewrite LASO-Computer as native Windows C++`](https://github.com/whiskywater/LASO-Computer/pull/1), is a separate open draft at `f631df2262efbe93d81ed4cf304d8b90992b95e8`; its code is not part of public `main`. The draft proposes a Windows C++20 endpoint and reports successful noninteractive Windows build/test checks. Its own documentation says the LASO worker protocol and approval exchange are not verified against current LASO. Its Playwright MCP adapter passes initialization, tool listing, and browser attachment probes, but its first browser operation fails in the AppContainer at Node path resolution. It does not report a successful LASO-to-Computer page action.

This private implementation remains idiomatic Go. This comparison uses public behavior and documentation as reference only; no public source was copied.

## Behavioral comparison

| Area | Public main / draft | Private Go implementation and disposition |
| --- | --- | --- |
| LASO transport and assignment | Public `main` has no runtime. The draft has local JSON-lines processing but says current LASO protocol compatibility and approval framing are unverified. | Implemented as supervised LASO process-worker v1 over stdio, with bounded frames, correlated work, cancellation, and rejection of results completed after cancellation. Private implementation is ahead; no port needed. |
| Endpoint identity and enrollment | Public `main` has no runtime; the draft says it has no remote enrollment or endpoint transport. | Persistent random local client identity; no hardware-derived identity or remote enrollment. Equivalent limitation is retained. |
| Local permission control | Draft defaults capabilities to deny and fails closed for `require_approval`, without sending permission-request frames. | Named capabilities use local allow/require-approval/deny policy and the existing LASO permission-request protocol. This private path is retained. |
| Desktop actions | Draft proposes bounded screen capture, pointer, keyboard, Unicode clipboard, window enumeration/focus, UI Automation, and restricted process execution. | Go implementation provides its current Windows capability set: screen capture, pointer and keyboard input, plain-text clipboard, window list/focus, browser navigation, and allowlisted process execution. The C++ draft is not merged; no additional capability was added. |
| Browser automation | Draft's AppContainer Playwright adapter has not completed a page operation and documents a path-access failure. | Private Go implementation has no Playwright/MCP browser provider. Defer until the upstream behavior is validated end-to-end and a Go-compatible permission, isolation, and protocol design is proven. |
| Bounds, cancellation, and audit | Draft documents bounded frames/results, cancellation, endpoint policy, and redacted audit events; its current protocol integration is incomplete. | Private transport and handlers have explicit frame/result bounds, duplicate protection, cooperative cancellation, default-deny policy, and redacted local audit. These protections remain unchanged. |
| Remote service, TLS, recovery | Draft states no remote enrollment, network transport, lease/reconnect protocol, or service package. | Private client is locally supervised; remote enrollment and endpoint leases are absent. No public-main implementation exists to port. |

The draft's proposed UI Automation provider and Playwright integration are not treated as released public features. They need successful Windows interactive acceptance, protocol compatibility, and tests showing permission denial, cancellation, restart, and bounded failure before adoption. In particular, process isolation alone is not a claim of OS sandboxing; the public draft's AppContainer restrictions are distinct from the private Go worker process boundary.

## Validation scope

Because no public-main feature was missing and no runtime code changed, this sync adds no Go port and no Windows interaction. Existing private CI remains the validation source for the Go implementation. The public draft's four passing Windows checks establish that its submitted C++ build/test jobs pass; they do not establish the draft's blocked browser operation or an end-to-end LASO integration. No public repository, PR, branch, release, or issue was modified.
