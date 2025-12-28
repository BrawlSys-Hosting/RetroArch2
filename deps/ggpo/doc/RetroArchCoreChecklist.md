# RetroArch Core Checklist for GGPO Delta

Use this as a practical checklist for getting a RetroArch core working well
with this GGPO fork.

## Build and Wiring
- [ ] Link the core against this GGPO build (not system GGPO).
- [ ] Use the public GGPO API calls (`ggpo_start_session`, `ggpo_add_player`,
  `ggpo_add_local_input`, `ggpo_synchronize_input`, `ggpo_advance_frame`).
- [ ] Ensure you call `ggpo_idle` once per frame with a reasonable time budget.
- [ ] Confirm you pass the correct `input_size` for your core's input struct.
- [ ] Confirm player count and player handles match the session setup.

## Determinism
- [ ] The simulation is deterministic across runs and machines.
- [ ] RNG state is deterministic and included in save/load.
- [ ] No wall-clock time influences simulation outcomes.
- [ ] Floating-point usage is deterministic (or avoided) across platforms.
- [ ] Frame-to-frame state is fully serialized (including hidden static state).

## State Save/Load
- [ ] `save_game_state` captures all simulation state needed to restore the frame.
- [ ] `load_game_state` restores exactly the state produced by `save_game_state`.
- [ ] `free_buffer` correctly frees any memory allocated by `save_game_state`.
- [ ] Use the reuse buffer if provided (`*buffer` non-null and `*len` > 0) to
  avoid per-frame allocations.
- [ ] Validate your save size is stable and doesn't exceed the provided buffer.

## Frame Execution
- [ ] `ggpo_synchronize_input` is called every frame, including during rollback.
- [ ] Use the returned inputs to advance the simulation (not raw local inputs).
- [ ] Rendering and audio are decoupled from simulation so rollback can run
  without rendering.
- [ ] Any rollback side effects (effects, audio events) are deferred or gated.

## Networking Behavior
- [ ] Frame delay is set via `ggpo_set_frame_delay` to balance latency vs rollback.
- [ ] Disconnect timeouts use `ggpo_set_disconnect_timeout` and
  `ggpo_set_disconnect_notify_start` as needed.
- [ ] Packet shaping is tuned if necessary:
  - [ ] `ggpo.network.max_input_bits` to cap packed input bits per packet.
  - [ ] `ggpo.network.send_interval` to reduce send rate bursts.
  - [ ] `ggpo.network.delay` and `ggpo.oop.percent` only for testing.

## Compression and Performance
- [ ] Async compression enabled (Sync::Config `async_compress = 1`).
- [ ] LZ4 acceleration set (`Sync::Config lz4_accel` or env var
  `ggpo.sync.lz4_accel`).
- [ ] Save-state buffers are reasonably sized; large state blobs increase
  compression and bandwidth cost.
- [ ] Avoid copying state buffers unnecessarily in the core.

## Diagnostics and Validation
- [ ] Use `ggpo_start_synctest` during development to catch desyncs early.
- [ ] Track `GGPOStateStats` to ensure compression queues are healthy.
- [ ] Track `GGPONetworkStats` to validate ping and send queue behavior.
- [ ] Log errors in save/load callbacks; a failed save/load is fatal for sync.

## Runtime Compatibility
- [ ] Fixed timestep simulation (RetroArch frame pacing should not alter logic).
- [ ] If using threads, ensure sim state is fully synchronized and deterministic.
- [ ] Avoid relying on platform-specific data sizes or endianness.
