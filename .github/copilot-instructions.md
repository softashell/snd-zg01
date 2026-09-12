# AI coding agent instructions

## Project overview

Linux kernel driver for the Yamaha ZG01 USB audio interface (VID 0x0499,
PID 0x1513). The device combines proprietary audio framing and vendor
requests with class-style clock controls. Evidence includes Windows ETW,
Linux USB traces, and hardware trials. One module `snd-zg01`,
one ALSA card, three PCM devices (Game Out, Voice Out, Voice In).
Distributed through DKMS packages for Debian and Arch.

## Architecture

### Single module, single card

`obj-m := snd-zg01.o` links four objects:

- `src/zg01_usb.c`: probe/disconnect, card creation, suspend/resume
- `src/zg01_pcm.c`: PCM ops, URB chains, the shared-endpoint data path
- `src/zg01_control.c`: vendor init request (bRequest 7, checks 0x80bb00)
- `src/zg01_usb_discovery.c`: descriptor dump, debug only, best effort

Build from the repository root only; `make -C src` fails. `snd_card_new`
allocates `struct zg01_dev` as card private data. Never free it manually;
`card->private_free` handles it. One probe on interface 1 creates the card
and all three PCM devices.

### Shared endpoint 0x01

Game Out and Voice Out both target EP 0x01 OUT. One URB chain (`out_chain`)
serves both PCM devices; Voice In has its own `in_chain` on EP 0x81.
Chain state is an explicit enum (`enum zg01_chain_state`) under
`dev->lock`; `inflight` is URB accounting. The driver mixes the Game and
Voice samples into separate frame slots, so the
two playback PCMs stay separate sinks.

- Either playback PCM alone supplies audio in its own frame slots.
- When both playback PCMs run, the pump copies both buffers into each
  200–280-byte packet. Inactive slots contain silence.
- Capture owns OUT as well as IN, including when capture joins playback.
  Prepare allocates both chains; START starts or adopts them.
- Playback-only runs OUT without IN by default. Optional priming briefly
  runs IN assist, armed only during a fresh OUT initialization.
- Normal playback STOP can hold OUT for `quiesce_ms` (default 3000).
  `keepalive_ms` (default 600000) restarts or adopts BOTH chains across close
  when capture is neither open nor running. Arm expiry after chain starts.
  An existing paired hold survives drain-final STOP, which schedules expiry.
  Without that hold, drain-final STOP stops OUT immediately.
- Hold demand must survive DRAINING while chain_start joins cleanup.
  Failed starts and capture takeover release both flags. Suspend/disconnect
  block late arming. Expiry cannot revoke a hold adopted by live playback.
- A runtime write of 0 to `keepalive_ms` takes effect at the next close/hw_free:
  that path revokes driver-owned demand before the no-consumer stop checks.
- The storm holdback is an eligible nominal-fallback state, so a cleared cached
  plan never strands RUNNING OUT at zero submissions. DRAINING blocks submission.
- The spread budget ignores a DRAINING consumer: a short terminating stream must
  not shrink a healthy RUNNING sibling's plan. A lone draining stream still
  spreads, which keeps its own tail hole away.
- Assist and prime release clear only their own IN ownership. Keep IN while real
  capture (`enabled`) or `in_hold` owns it; never test mutex-only `running` from
  the pump, which runs under `dev->lock`.
- The idle-fault re-arm delay is per-device under `dev->lock`: it starts at
  `keepalive_rearm_ms`, doubles to `keepalive_rearm_max` with capped
  arithmetic, and resets only on an explicit successful START or a PM reset —
  never on an automatic keepalive restart.
- `WQ_PERCPU` exists only from v6.17. `ZG01_WQ_PERCPU` guards it with a version
  test; `#ifdef` cannot detect an enum constant.
- `prime_ms` defaults to 0. Enabling it drops application frames during
  silence priming. Valid IN plus 50 ms is an experimental release heuristic,
  not proof of audible readiness. Warm adoption does not re-arm assist.

### Packet formats

Playback packet: 200–280 bytes = 5–7 frames of 40 bytes when IN plans
supply pacing. Without IN, free-run uses nominal six-frame packets.
The IN endpoint reports 5–7 frames per packet (~21 ppm observed drift).
Frame: Voice_L(4),
Voice_R(4), Game_L(4), Game_R(4), 24 pad bytes. 32 ISO packets per URB,
4 ms per URB, `MAX_URBS` 16 (64 ms buffering).

Capture packet: variable, 5-7 frames of 16 bytes plus an 8-byte header
(counter + length) and 4-byte trailer; 108 bytes nominal, 124-byte
buffer. When IN runs, the pump follows validated frame-count plans.
Fresh IN startup resets its observation window without
resetting live OUT submissions. Playback-only free-run is not starvation.

### Rates

All three PCMs run at 48 kHz only; the old 16 kHz capture mode is gone.
The device clock is shared across interfaces; the driver always
initializes it at 48 kHz. The vendor handshake
in prepare resets both interfaces to alt 0, so it runs only when no chain is
streaming (`device_initialized` flag plus chain-state checks under
`state_mutex`). Initialization failures return to ALSA without setting
`device_initialized`. Check control lengths, interface results, and the
48 kHz clock readback before publishing success.

### Streaming lifecycle

Only STARTING/RUNNING chains may submit URBs. Stop and fault paths publish
DRAINING under `dev->lock`; cleanup joins callbacks before publishing STOPPED.
Held OUT chains send silence when no playback consumer runs. Cleanup is deferred to
`zg01_cleanup_wq`; period elapsed runs on `zg01_period_wq` (nonatomic PCM).
Suspend stops both chains and forces re-init on the next prepare;
`reset_resume` covers firmware loss across suspend.

## Pitfalls

- `runtime->buffer_size` is in frames, not bytes. Do not divide by
  `bytes_per_frame`.
- Keep the interface setup out of open paths. The Magic Sequence kills live
  URBs; prepare requires both chains STOPPED before running it.
- USB control buffers must be heap allocated, never on stack.
- IN errors have bounded startup tolerance. After valid feedback begins,
  strict mode faults invalid IN immediately. `in_error_grace_ms` (default 100,
  cap 500) tolerates transient packet errors, never malformed timing plans.
  Terminal resubmit statuses still drain. See `docs/STARTUP_RECOVERY.md`.
- Do not count free-run completions as feedback starvation. Count a transport
  fault once per OUT fault latch, but preserve notifications to real capture.
- Count `silence_frames` from the final submitted plan, never from the requested
  total: frames the spread withheld are `even_fill_deficit`, and `short_hold`
  sends a repeated frame instead of a zero one.
- Keep workqueue flags compatible with the oldest supported kernel. The project
  advertises Debian/Ubuntu builds on matching headers, and `WQ_PERCPU` does not
  exist before v6.17.
- The Makefile auto-detects clang kernels via `CONFIG_CC_IS_CLANG`. Do not
  hardcode `LLVM=1`.
- Do not add `EXPORT_SYMBOL` for intra-module symbols.

## Build and test

```bash
make                                  # from repo root
tests/test-kernel-build.sh            # strict W=1 build check
tests/test-zg01-lifecycle.sh          # open/close stress via pw-cat
tests/test-arch-packaging.sh          # PKGBUILD and .SRCINFO consistency
```

Test on real hardware when possible. PipeWire probing is the most sensitive
area: watch for trigger loops with `journalctl -b -k --grep zg01`.

## References

- `docs/PROTOCOL_CAPTURE.md`: capture workflow for new control messages
- `docs/INITIALIZATION_ANALYSIS.md`: device USB topology and packet formats
- `src/zg01.h`: shared structs, chain state, packet constants
