# Security model

## Authorization and capabilities

Each invocation goes through the endpoint policy before a provider is called. The default decision is `deny`; LASO authorization cannot enable an action that endpoint policy denies. Capabilities are named individually, including `screen.capture`, `pointer.move`, `pointer.click`, `keyboard.type`, `keyboard.key`, `clipboard.read`, `clipboard.write`, `window.list`, `window.focus`, `ui.inspect`, `ui.invoke`, `browser.*`, and `shell.execute`.

The implemented policy decisions are `allow`, `deny`, and `require_approval`. `require_approval` currently fails closed because the public LASO revision inspected does not document a correlated worker-request exchange. LASO-Computer does not emit arbitrary unsolicited frames between protocol requests. No approval should be described as integrated until tested against an upstream contract.

## Plugins and adapters

Provider registration never grants permission. An untrusted provider's capabilities remain subject to the same endpoint policy and argument/result bounds. The registry exposes per-provider identity, version, declared capability schemas, health, invocation, cancellation, and lifecycle. Native providers currently run in-process; an in-process C++ provider is trusted code and therefore must come from this reviewed build. Arbitrary dynamic DLL loading is not supported.

The Playwright MCP adapter is a separate AppContainer process with a fixed operation set, bounded protocol I/O, timeout/cancellation, explicit stdio handles, and a kill-on-close Job Object capped at one process. C++ launches and owns Edge separately with its normal Chromium sandbox, a managed per-run profile, a loopback-only CDP port, and a separate kill-on-close Job Object. The user Edge profile is never selected. MCP receives read/execute access only to the configured Node directory and pinned package tree, plus read/write/execute/delete on its unique provider temp directory; it cannot access the Edge profile. Added provider ACL entries are revoked on shutdown. The Node AppContainer has no network capabilities.

For this investigation only, an AppContainer-SID-specific loopback exemption allowed MCP to attach to the C++-owned Edge. It is temporary machine state and must be removed after testing; Windows scopes it to the AppContainer SID, not to one port. The provider now passes initialize, tool listing, and its `browser_tabs` attach probe after a local `\\.\\pipe\\LOCAL\\` named-pipe namespace patch to the installed pinned package. Actual browser operations remain blocked: Node's emulated and native `realpath` fail because `lstat` on the absolute output path's ancestors is denied. Only traversal is currently granted on those profile ancestors; narrowly testing `FILE_READ_ATTRIBUTES | FILE_TRAVERSE` requires administrator approval. No directory listing or file-data permission is justified. No broad volume read is granted. Browser capabilities must remain unavailable until the clean-install pipe patch and real browser operations pass. CDP itself has no authentication and a same-user local process that discovers the ephemeral port could attach. Arbitrary Playwright code execution, cookies, storage, uploads, and downloads are not exposed. Node's main-entry `lstat('C:\\')` probe is avoided with `--preserve-symlinks-main`.

## Process execution

`shell.execute` is a legacy capability name; it executes a program directly and never invokes a shell. It requires an exact absolute executable path allowlist. Arguments have count/size limits, command-line quoting follows Windows process rules, the child receives only explicitly allowed environment variables, the working directory is a controlled temporary directory, output is bounded and marked if truncated, and timeout/cancellation terminates the child job. UNC and reparse-point path surprises are rejected where checked. Broad executable allowlists are unsafe.

## Validation and data bounds

The worker transport enforces a 1 MiB maximum JSON frame centrally on reads and writes. Malformed JSON, duplicate/conflicting request identifiers, invalid payloads, and oversized frames fail as protocol errors. Structured data and capability results are separately bounded. Clipboard and structured UI reads fail if their limit is exceeded; process output carries explicit truncation metadata.

## Audit and privacy

Audit events attribute caller/session/job, provider, capability, policy decision, outcome, and timing. They do not include user arguments, typed text, clipboard values, URLs, screenshots, or process stdout/stderr. Audit storage is local. A host administrator can still inspect process memory and endpoint files; this design does not claim protection from a compromised Windows account or administrator.

## Transport and limitations

There is no inbound listener and no remote endpoint enrollment. The supported operating model is a locally supervised process with stdin/stdout. The exact protocol contract has not yet been validated against current public LASO: the inspected LASO `main` commit `ef072429c96eb9b0964e56a561591a37ccb68fbd` did not contain its worker-process protocol documentation or implementation. Do not connect this build to production LASO or claim end-to-end compatibility until the contract and approval exchange are published and an acceptance lane passes.
