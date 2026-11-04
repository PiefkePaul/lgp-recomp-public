# Horizon IPC (HIPC/CMIF) and `sm` in an own runtime

Knowledge across games from P3.3 (`runtime/hle/`, kernel part in `runtime/core/src/kernel.cpp`). Sources are the publicly documented formats (switchbrew, libnx headers); no code was taken over from another project.

## How the runtime models IPC

- **Services are HLE and run synchronously on the calling guest thread.** `svcSendSyncRequest` calls `SessionHandler::handle_request` directly; there is no server thread, no `svcReplyAndReceive` and no server session in the kernel. That is enough for a game that is only a client. Services that have to block (such as an event that is waited on later) hand out event handles; the call itself does not block.
- **Kernel side** (`lgp::rt::Kernel`): `register_named_port(name, connect)` and `create_session(handler)` (`KClientSession` in the handle table). SVCs: `ConnectToNamedPort` (0x1F, x1 = pointer to the name, at most 11 characters + NUL, otherwise `OutOfRange`; unknown: `NotFound` 0xF201), `SendSyncRequest` (0x21, message in the first 0x100 bytes of the thread's TLS), `SendSyncRequestWithUserBuffer` (0x22, address and size page-aligned, otherwise `InvalidAddress`), `GetProcessId` (0x24).
- **Unknown commands stop the guest** (`SvcOutcome::Unimplemented` with a log), just like an unimplemented SVC. It never answers with a guessed value.
- The runtime copies the message (up to 0x10000 bytes) under the memory mutex into a host buffer, parses it, calls the service without the memory mutex (services access memory themselves via `copy_from_user`/`copy_to_user`) and writes the reply back.
- **PID:** if a request asks for the PID (special header bit 0), the kernel inserts `ProcessConfig::process_id`; what the client wrote there does not count. The default value 0x51 is a placeholder, not measured.

## HIPC layout (message from the start of the buffer, 32-bit words)

| Part | Content |
|---|---|
| Word 0 | type (bits 0–15), count X (16–19), A (20–23), B (24–27), W (28–31) |
| Word 1 | data words (0–9), recv static mode (10–13), recv list offset in words (20–30), special header present (31) |
| Special header | bit 0 send PID, bits 1–4 copy handles, bits 5–8 move handles; then PID (u64), copy, move handles |
| X descriptor (2 words) | w0: index (0–5), address bits 36–41 (6–11), bits 32–35 (12–15), size (16–31); w1: address bits 0–31 |
| A/B/W (3 words) | size bits 0–31; address bits 0–31; w2: mode (0–1), address bits 36–57 (2–23), size bits 32–35 (24–27), address bits 32–35 (28–31) |
| Raw data | `num_data_words` words from the first word after the descriptors |
| C descriptors (2 words) | address bits 0–31; address bits 32–47 (0–15) and size (16–31). Located at the recv list offset or, if that is 0, directly after the raw data. Mode 2 = one descriptor, mode n > 2 = n − 2 descriptors |

Types: 2 Close (no reply), 4 Request, 5 Control, 6/7 the same "WithContext".

**Alignment pitfall:** the CMIF part starts at the first 16-byte boundary **relative to the start of the message** (the TLS is 0x200-aligned), not relative to the start of the raw data. libnx always reserves 16 bytes of padding for this in `num_data_words`. The reply counts the padding in `num_data_words` too. Concretely: without handles CMIF sits at 0x10, with one move handle (8 + 4 + 4 = 0x10) also at 0x10, with a PID request at 0x20.

## CMIF

- Input: `SFCI` (0x49434653), version, command, token; then the parameters like a C struct (natural alignment). Output: `SFCO` (0x4F434653), version, result, token; then the return values.
- The runtime answers a wrong magic with `sf InvalidInHeader` (module 10, description 211); the session stays usable.
- **Output objects** (sub-interfaces): without a domain as move handles to new sessions, namely **before** the explicit move handles of the command (libnx order). In a domain as object IDs directly after the payload, without a handle.
- **Control** (type 5): 0 `ConvertCurrentObjectToDomain` (returns the ID of the existing object, 1 in the runtime), 1 `CopyFromCurrentDomain` (ID → normal session handle), 2 `CloneCurrentObject`, 3 `QueryPointerBufferSize` (u16), 4 `CloneCurrentObjectEx`. Control replies never have a domain header. The clone of a domain session is a second session on **the same** domain (same object IDs, new objects are visible on both), as in Horizon's sf; libnx' `fsInitialize` opens several fsp-srv sessions this way. In the runtime the clones also share the lock, so requests to the same domain run one after the other.
- **Domain request:** 16-byte header before `SFCI`: type (1 message, 2 close object), number of in-objects, length (CMIF header + payload), object ID, padding, token. In-object IDs follow after this length. Unknown ID: `sf TargetNotFound` (10/261). Domain reply: 16-byte header (number of out-objects), then `SFCO`.

## `sm:`

- Every connection to `sm:` is its own instance with its own `RegisterClient` state.
- Command 0 `RegisterClient` (PID via special header, u64 reserved). Command 1 `GetServiceHandle` (name as 8 bytes, padded with zeros) returns a move handle. Without a prior `RegisterClient`: `sm InvalidClient` (module 21, 2). Unknown service: `sm NotRegistered` (21/7 = 0xE15); the runtime logs this as a warning instead of stopping, because games query optional services this way.
- `ServiceManager::register_service(name, factory)`: every `GetServiceHandle` request gets a new service instance. Services with shared state hold it themselves (e.g. in a `shared_ptr` that the factory captures).

## Tests and acceptance test

- `runtime.ipc_lifted(_checked)`: `ipc_main` in `tests/runtime/thread_programs.s` builds the messages itself in the TLS (like libnx' `cmifMakeRequest`), connects to `sm:`, calls `RegisterClient` and `GetServiceHandle("lgp:test")` and then command 0 of the service (2 + 3 = 5), sends Close and closes the handles. Translated by the P2.3 lifter, in baton and core baton. Negative cases: service not registered (`NotRegistered` in step 3), no `sm:` port (`NotFound` in step 1).
- `runtime.ipc_hle(_checked)`, native and in Node: parser (all descriptor types, truncated messages), reply layout, `sm:` error cases, PID replacement, A→B buffers, X/C descriptors, wrong magic, unknown commands, sub-interfaces as handles, Clone (also of a domain session), `QueryPointerBufferSize`, Close message, domain with sub-objects, in-objects, `CopyFromCurrentDomain` and closing objects.
- **Limit:** the acceptance test runs with hand-coded A64, not with a libnx build. A real libnx fixture needs the loader (P2.1/P2.8); that is the catch with P3.1.

## Open points

- No `svcReplyAndReceive`/server side: services that the game offers itself normally do not exist for applications.
- `pointer_buffer_size()` reports 0x8000 everywhere. Real values per service are not measured; this only becomes relevant when a client sizes its C buffer by it.
- `svcSendAsyncRequestWithUserBuffer` is missing.
