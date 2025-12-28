# GGPO Stats and Control Levers

This document covers every exposed stat field and every runtime/build-time lever
available in this fork. References use the public API names and the relevant
config keys used by the codebase.

## Stats

### Network stats (GGPONetworkStats via ggpo_get_network_stats)
- network.send_queue_len: number of input frames queued for send but not yet
  acked by the peer (rough indicator of RTT or loss).
- network.recv_queue_len: intended to be the count of buffered remote inputs
  pending validation; not currently populated in this fork.
- network.ping: round-trip time in ms, derived from quality report ping/pong.
- network.kbps_sent: estimated outbound bandwidth in KB/s including UDP header.
- timesync.local_frames_behind: how many frames the local client is behind the
  remote estimate. Positive means local is behind.
- timesync.remote_frames_behind: how many frames the remote client reports it
  is behind. Positive means remote is behind.

### State stats (GGPOStateStats via ggpo_get_state_stats)
- delta_frames: count of delta-compressed frames saved so far.
- keyframes: count of full (non-delta) frames saved so far.
- delta_ratio_last: compression ratio (%) for the most recent delta frame.
- delta_ratio_avg: average compression ratio (%) across delta frames.
- delta_ratio_max: max compression ratio (%) seen for delta frames.
- compress_job_queue_len: current queued async compression jobs.
- compress_result_queue_len: current queued async compression results.
- compress_pending_count: saved frames still waiting for async compression.
- compress_job_queue_max: high-water mark for job queue since thread start.
- compress_result_queue_max: high-water mark for result queue since thread start.

Notes:
- Queue stats only apply when async compression is enabled.
- High-water marks reset when the compression thread is restarted.

## Control levers

### Runtime API (public)
- ggpo_set_frame_delay(player, frames): per-player input delay (latency tradeoff
  vs rollback severity).
- ggpo_idle(timeout_ms): budget for GGPO internal work (packet IO, resend, stats).
- ggpo_set_disconnect_timeout(timeout_ms): disconnect if no packets in window.
- ggpo_set_disconnect_notify_start(timeout_ms): emit network interrupted event
  after this much silence, before full disconnect.
- ggpo_start_session(..., num_players, input_size, local_port): input_size is a
  direct bandwidth driver; num_players changes queue sizes and validation cost.
- ggpo_start_synctest(..., frames): how many frames between determinism checks.

### Runtime config (environment variables via Platform::GetConfigInt)
- ggpo.sync.lz4_accel: LZ4 acceleration for state compression. Higher is faster
  with worse ratio; lower is slower with better ratio. Defaults to 2 if unset.
- ggpo.sync.prediction_frames: prediction window length (0 uses default build
  value; clamped to MAX_PREDICTION_FRAMES).
- ggpo.network.delay: artificial outbound latency/jitter for testing.
- ggpo.oop.percent: percent chance to send an out-of-order packet for testing.
- ggpo.network.send_interval: minimum ms between input packet sends (0 disables).
- ggpo.network.max_input_bits: cap on packed input bits per packet. Values <= 0
  fall back to MAX_COMPRESSED_BITS - 1.

### Integration config (Sync::Config)
- num_prediction_frames: prediction barrier length (lower reduces rollback window
  but may reject inputs more often).
- lz4_accel: overrides ggpo.sync.lz4_accel when > 0.
- async_compress: enables the async compression worker (1) or forces sync (0).

### Build-time constants (requires rebuild; must match across peers)
- MAX_PREDICTION_FRAMES (ggpo/src/lib/ggpo/sync.h): ring buffer depth for saved
  states and the upper bound on prediction.
- GGPO_STATE_KEYFRAME_INTERVAL (ggpo/src/lib/ggpo/sync.h): frequency of full
  keyframes. Lower means more keyframes, higher means longer delta chains.
- GGPO_MAX_PLAYERS and GGPO_MAX_SPECTATORS (ggpo/src/include/ggponet.h): hard
  caps for session sizing.
