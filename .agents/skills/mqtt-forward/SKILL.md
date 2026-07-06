---
name: mqtt-forward
description: Repository-specific guidance for working in the mqtt-forward C project, which tunnels TCP traffic over MQTT with per-session sequencing, ack, retransmit, and beacon-based discovery. Use when Codex needs to explain the codebase, debug protocol or session behavior, change CLI or runtime behavior, add tests, or validate fixes in this repo.
---

# MQTT Forward

Understand this repository as a small C system with one executable, a custom TCP-over-MQTT framing protocol, and two levels of testing: focused unit tests plus a Docker-backed integration flow.

Start by reading [references/repo-map.md](references/repo-map.md). Use it as the current map of binaries, source ownership, topic flow, and validation commands.

## Workflow

1. Read `CMakeLists.txt`, `README.rst`, and the specific source files named in `references/repo-map.md`.
2. Match the request to the correct layer before editing:
   - CLI, MQTT setup, thread startup, message dispatch: `src/mqtt-forward.c`
   - session creation, cleanup, socket shutdown, MQTT subscription setup: `src/session.c`
   - frame structs and remote-config encoding: `src/protocol.h`, `src/protocol.c`
   - beacon discovery: `src/beacon.c`
   - log behavior and server/client prefixes: `src/log.c`, `src/log.h`
   - IDs and small helpers: `src/utils.c`
3. Keep changes narrow and consistent with the existing style: simple C, pthreads, global session state, and explicit error handling.
4. Validate with the smallest meaningful test first, then widen only if the change crosses module boundaries.

## Working Rules

- Prefer repo-native commands and scripts over inventing new harnesses.
- Treat `README.rst` as user-facing intent, but trust the source and tests for current behavior.
- When touching sequencing, retransmit, backlog, or shutdown logic, inspect both `src/mqtt-forward.c` and `src/session.c`; behavior is split across them.
- When touching CLI options or defaults, update both code and any affected examples or tests.
- When touching discovery behavior, review both `src/beacon.c` and the MQTT topic parsing paths in `src/mqtt-forward.c`.

## Validation

- Build with CMake:
  ```bash
  cmake -S . -B build
  cmake --build build
  ```
- Run unit tests for local logic changes:
  ```bash
  ctest --test-dir build --output-on-failure
  ```
- Use the dedicated helper binaries and scripts for broader confidence:
  - `./build/mqtt-forward-unit-tests`
  - `./test/run-multi-client-echo-test.sh`
  - `./test/run-multi-client-echo-test.sh --unsecure`
- Expect the integration scripts to require Docker, a built tree, and local ability to launch the Mosquitto test environment.

## Current Hotspots

- `handle_mqtt_message()` in `src/mqtt-forward.c` is the main receive-side coordinator.
- `tcp_session_rx_thread_fn()` in `src/mqtt-forward.c` owns transmit framing, heartbeats, ack propagation, and retransmit behavior.
- `request_session_close()` and `clear_session()` in `src/session.c` govern shutdown and lifetime tracking for old session IDs.
- `handle_remote_config()` validates client-provided remote IP/port overrides before server-side session creation.

## Deliverables

- When asked to explain the repo, summarize the tunnel architecture, MQTT topics, session lifecycle, reliability layer, and available tests.
- When asked to change behavior, say which source layer changed and which validation path you used.
