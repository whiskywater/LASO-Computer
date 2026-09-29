# Security model

## Authorization and capabilities

Each invocation goes through the endpoint policy before a provider is called. The default decision is `deny`; LASO authorization cannot enable an action that endpoint policy denies. Capabilities are named individually, including `screen.capture`, `pointer.move`, `pointer.click`, `keyboard.type`, `keyboard.key`, `clipboard.read`, `clipboard.write`, `window.list`, `window.focus`, `ui.inspect`, `ui.invoke`, `browser.*`, and `shell.execute`.

The implemented policy decisions are `allow`, `deny`, and `require_approval`. `require_approval` currently fails closed because the public LASO revision inspected does not document a correlated worker-request exchange. LASO-Computer does not emit arbitrary unsolicited frames between protocol requests. No approval should be described as integrated until tested against an upstream contract.

## Plugins and adapters

Provider registration never grants permission. An untrusted provider's capabilities remain subject to the same endpoint policy and argument/result bounds. The registry exposes per-provider identity, version, declared capability schemas, health, invocation, cancellation, and lifecycle. Native providers currently run in-process; an in-process C++ provider is trusted code and therefore must come from this reviewed build. Arbitrary dynamic DLL loading is not supported.

The Playwright MCP adapter is a separate AppContainer process with an explicit per-run browser profile, no inherited credentials, a fixed operation set, bounded protocol I/O, timeout/cancellation, and a kill-on-close Job Object. Its AppContainer receives read/execute access only to the configured Node directory and pinned package tree, and read/write/execute access to its unique per-run temp/profile directory. Installed Edge retains its existing `ALL APPLICATION PACKAGES` read/execute access; LASO-Computer does not modify the browser installation ACL. Added AppContainer ACEs are revoked on shutdown. Arbitrary Playwright code execution, cookies, storage, uploads, and downloads are not exposed. Node's `lstat('C:\\')` main-module resolution probe is avoided with `--preserve-symlinks-main`; no volume-root ACL is added. The MCP handshake succeeds, but Edge creation with Playwright's remote-debugging pipe stalls, so a real browser startup probe fails closed and capabilities remain unavailable. Do not weaken the sandbox by granting broad filesystem access or disabling Chromium's own sandbox without a tested replacement boundary.

## Process execution

`shell.execute` is a legacy capability name; it executes a program directly and never invokes a shell. It requires an exact absolute executable path allowlist. Arguments have count/size limits, command-line quoting follows Windows process rules, the child receives only explicitly allowed environment variables, the working directory is a controlled temporary directory, output is bounded and marked if truncated, and timeout/cancellation terminates the child job. UNC and reparse-point path surprises are rejected where checked. Broad executable allowlists are unsafe.

## Validation and data bounds

The worker transport enforces a 1 MiB maximum JSON frame centrally on reads and writes. Malformed JSON, duplicate/conflicting request identifiers, invalid payloads, and oversized frames fail as protocol errors. Structured data and capability results are separately bounded. Clipboard and structured UI reads fail if their limit is exceeded; process output carries explicit truncation metadata.

## Audit and privacy

Audit events attribute caller/session/job, provider, capability, policy decision, outcome, and timing. They do not include user arguments, typed text, clipboard values, URLs, screenshots, or process stdout/stderr. Audit storage is local. A host administrator can still inspect process memory and endpoint files; this design does not claim protection from a compromised Windows account or administrator.

## Transport and limitations

There is no inbound listener and no remote endpoint enrollment. The supported operating model is a locally supervised process with stdin/stdout. The exact protocol contract has not yet been validated against current public LASO: the inspected LASO `main` commit `ef072429c96eb9b0964e56a561591a37ccb68fbd` did not contain its worker-process protocol documentation or implementation. Do not connect this build to production LASO or claim end-to-end compatibility until the contract and approval exchange are published and an acceptance lane passes.
