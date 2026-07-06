# mqtt-forward Repo Map

## Purpose

`mqtt-forward` is a C command-line tool that forwards TCP traffic, commonly SSH, through an MQTT broker. One instance runs client-side and accepts local TCP connections; the other runs server-side and connects to the actual target TCP service.

The reliability layer is implemented above MQTT. Frames carry sequence numbers, optional ack numbers, flags, and optional remote-config data so the peers can reorder data, retransmit lost packets, and override server-side target IP/port from the client side.

## Build and Test Surface

- Main binary: `build/mqtt-forward`
- Unit-test binary: `build/mqtt-forward-unit-tests`
- Test helper binary: `build/mqtt-forward-test-helper`
- CTest entry: `ctest --test-dir build --output-on-failure`
- Integration script: `test/run-multi-client-echo-test.sh`
- Full broker/bootstrap scripts: `test/start-all.sh`, `test/start-mqtt-forward.sh`, `test/setup-mqtt-docker.sh`, `test/start-mqtt-docker.sh`, `test/teardown.sh`

`CMakeLists.txt` enables `CTest`, exports `compile_commands.json`, and builds:

- `mqtt-forward`
- `mqtt-forward-unit-tests`
- `mqtt-forward-test-helper`

## Source Ownership

### `src/mqtt-forward.c`

Main entry point and the densest file in the repo.

Owns:

- CLI parsing and environment-variable handling
- default port/QoS/topic-prefix behavior
- TLS setup for Mosquitto
- MQTT reconnect loop and callback registration
- client-side TCP listener setup
- per-session TCP receive thread
- inbound MQTT topic parsing and dispatch
- receive-side backlog, ack, retransmit, and delivery logic
- server list mode and beacon transmit mode

Key functions:

- `tcp_session_rx_thread_fn()`
- `handle_mqtt_message()`
- `on_message()`
- `on_connect()`
- `mqtt_forward_init()`
- `mqtt_forward_start()`
- `main()`

### `src/session.c`

Session lifecycle and shared session state.

Owns:

- `tcp_sessions[]`, `session_mtx`, and `old_session_ids[]`
- creating client and server sessions
- MQTT subscription setup for client-created sessions
- server-side TCP connect logic
- orderly shutdown via `request_session_close()`
- cleanup and archival of old session IDs via `clear_session()`

Important current behavior:

- `request_session_close()` marks a session as closing and shuts down the socket to wake blocked I/O.
- `clear_session()` ignores already-cleared sessions and stores the old session ID in a bounded history.

### `src/protocol.h` and `src/protocol.c`

Defines the custom framing protocol:

- `tcp_over_mqtt_hdr`
- flag bits for disconnect, ack, no-data heartbeat, beacon, and remote config
- remote-config item format for IP and port overrides

`create_config_header()` builds the optional client-to-server config block included with the first outbound client frame.

### `src/beacon.c`

Tracks discovered server IDs and last-seen timestamps for `--list-servers`. Output now goes through the logging layer rather than raw `printf`.

### `src/log.c` and `src/log.h`

Central logging wrapper.

Important details:

- global `server_mode` lives here and controls `SERVER-SIDE:` vs `CLIENT-SIDE:` prefixes
- `-d` maps to `LOG_DEBUG` through `mqtt_forward_set_log_level()`
- most repo output flows through `LOG(...)`

### `src/utils.c`

Random ID generation:

- client IDs like `mqtt-forward-<12 hex chars>`
- session IDs with 32 hex characters

## Topic Layout

Default topic prefix: `ssh`

Patterns:

- `<prefix>/<server-id>/<session-id>/tx`
- `<prefix>/<server-id>/<session-id>/rx`
- `<prefix>/<server-id>/beacon/rx`

Client-created sessions publish to `.../tx` and subscribe to `.../rx`. Server-side sessions receive on `.../tx` and publish replies to `.../rx`.

## Important Runtime Modes

- Client mode:
  - listen on a local TCP port
  - create a new session per accepted TCP connection
  - optionally send remote target overrides with `--remote-ip` and `--remote-port`
- Server mode:
  - subscribe to `<prefix>/<server-id>/+/tx`
  - create a TCP connection to the target service when a new session appears
  - optionally emit beacons with `--beacon`
- Discovery mode:
  - `--list-servers`
  - subscribe to `<prefix>/+/beacon/rx`
  - print recently seen server IDs

## Current Validation Coverage

### Unit tests in `test/unit_tests.c`

Covers:

- sized string comparison helper behavior
- random ID format helpers
- remote-config header generation
- backlog cleanup helpers
- session close marking
- session cleanup and old-session tracking

Does not cover the full MQTT loop or end-to-end TCP forwarding.

### Integration helper in `test/mqtt_forward_test_helper.c`

Provides:

- local echo server
- multi-client concurrent test client
- timeout-aware send/receive helpers

### Multi-client integration script

`test/run-multi-client-echo-test.sh`:

- starts the Mosquitto test broker
- launches an echo server
- launches both mqtt-forward sides
- waits for broker connection log lines
- runs multiple concurrent clients through the tunnel

Use this when changes affect session concurrency, delivery ordering, or end-to-end forwarding.

## Common Change Heuristics

- CLI or startup bug: inspect `main()`, `mqtt_forward_init()`, and `README.rst`
- bad topic routing or unexpected session creation: inspect `on_message()` and `create_session()`
- retransmit, ack, or ordering bug: inspect `tcp_session_rx_thread_fn()` and `handle_mqtt_message()`
- shutdown, stale session, or socket-close bug: inspect `request_session_close()` and `clear_session()`
- remote target override bug: inspect `create_config_header()` and `handle_remote_config()`
