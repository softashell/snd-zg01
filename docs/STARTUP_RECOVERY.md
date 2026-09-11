# Startup and keepalive recovery

This describes the current experimental branch, not a hardware validation result.
A build or userspace harness cannot establish audible startup behavior.

## Transport ownership

Cold playback runs OUT without IN unless capture, priming, or timed assist requests IN.
Playback close can keep both chains active for `keepalive_ms` (default 600000).
The helper publishes both hold flags, joins any draining chain through `chain_start`,
and starts or adopts OUT and IN. It schedules expiry after both starts.

A failed start releases both hold flags and stops chains without real consumers.
Capture open releases both flags only after its constraint setup succeeds.
A failed capture open preserves the existing hold.
Playback can adopt a healthy hold without a new OUT epoch.
An expiry callback must not revoke a hold that running playback adopted.
The last playback STOP schedules expiry again, including an existing paired hold
at drain-final STOP. Without an existing hold, drain-final STOP stops OUT.

When playback is running, OUT already has a real owner, so keepalive arms an
IN-only hold (`in_hold` alone): it starts IN and unwinds on failure without
touching OUT. A capture close under running playback takes the same path.
Without this, a mid-playback IN death left the device deaf — RUNNING
transport, zero audio — because the ZG01 gates its output path on combined
IN+OUT liveness. Fault re-arm schedules only when real capture does not own IN.

Suspend blocks keepalive and new chain starts before suspending the PCMs.
Disconnect blocks late close/hw_free arming. Teardown joins earlier arming under
`state_mutex`, then cancels expiry work outside that mutex. An earlier helper
cannot leave a timer queued after teardown's final cancellation.

Disconnect also closes the streaming sessions: after the chains drain it sends
SET_INTERFACE alt 0 on interface 1 and then on interface 2, matching the Windows
stop trace (frames 104307/104331). A hot `rmmod` without that close left
firmware 1.50 wedged until a power cycle. Failures here are logged, not fatal.
Runtime buffers remain separate from chain buffers, so idle OUT sends
driver-owned silence without a PCM buffer.

Keepalive cannot establish IN before the first cold playback. It does not prove
that the device accepts the first audible frame after power-on.

## Initialization

Prepare publishes `device_initialized` only after successful initialization.
USB control requests must return the requested byte count. Interface changes
must succeed. The clock setter must succeed and the clock getter must report
48 kHz. The driver retries the clock pair up to three times.

The five early vendor discovery reads and the commit-handshake transfers are
advisory. Firmware 1.50 STALLs (`-EPIPE`) them unpredictably — the stalling
set varies per power session, and one validated session stalled all seven
vendor transfers while playback ran normally. Their payloads are unused.
Initialization sends the complete legacy sequence every attempt: no request
aborts the sequence, because a half-done sequence (interfaces left at alt 0,
commit writes unsent) makes the next attempt stall more. Failed advisory
transfers log a warning and the sequence continues.

Interface alt changes and the clock SET_CUR/GET_CUR pair are load-bearing.
The first of those failures is latched and returned after the sequence
completes; alt 1 is restored unconditionally so a failed attempt leaves the
device ready for the next one. The clock getter must report 48 kHz, retried
up to three times.

A failure returns an error to ALSA and leaves initialization eligible for retry.
The code does not silently accept a different clock rate or continue into PCM
streaming after a failed handshake. Existing request bytes and order remain unchanged.
Each initialization failure logs its failing stage (for example
`initialization failed at clock GET_CUR: -32`) instead of a bare
`set_rate failed` line.

## Idle-fault re-arm

A transient idle fault drops the hold pair and drains both chains. Without
recovery the device stayed cold for the rest of the session: one starvation
xrun left IN dead while OUT keepalive cycled on alone.

`keepalive_rearm_ms` (default 1000, 0 disables) schedules one keepalive
re-arm after an idle fault with no enabled stream, doubling its backoff per
fault up to `keepalive_rearm_max` (default 60000). The re-arm passes the
normal keepalive gate, so capture-open, suspend, and disconnect states still
refuse it. Enabled streams suppress the re-arm; their own recovery paths own
the next start. Suspend and disconnect clear the pending flag and cancel the
work item.

## Transient IN error experiment

`in_error_grace_ms` defaults to 0, which retains strict feedback error handling.
A nonzero value permits a bounded sequence of transient invalid IN URBs.
The implementation rounds to 4 ms URBs and caps the setting at 500 ms.
A valid full IN plan resets the consecutive-error count.

The grace covers successful empty packets and packet statuses `-EPROTO`,
`-EILSEQ`, `-ETIME`, and `-EXDEV`. It does not turn those packets into timing plans.
The pump uses its existing last-plan or nominal startup fallback.
Malformed nonempty packets retain strict handling. Terminal URB cancellation,
shutdown, and resubmit failure still drain the affected transport.

While the grace is active and IN is intentionally running — a real capture
consumer or the keepalive `in_hold` — transient IN packet errors never fault
the shared OUT transport at any bound. This covers both the IN-side
consecutive-error limit and the OUT pump gap-fallback. The pump free-runs on
its nominal cadence and logs `IN storm under capture: OUT free-running` once
per storm. The first valid plan lifts the holdback and restores feedback
pacing. The exemption is scoped to intentional IN: a capture-only rule turned
the IN-only re-arm into a two-second fault loop, because each re-armed
keepalive IN stormed and executed OUT through the IN-side limit.

For a hardware A/B, start with 0 versus 100 while keeping all other parameters,
clients, PCM buffer sizes, and test audio unchanged:

```bash
printf '100\n' | sudo tee /sys/module/snd_zg01/parameters/in_error_grace_ms
cat /sys/module/snd_zg01/parameters/in_error_grace_ms
```

Return to strict mode by writing 0. The user performs privileged changes.
Do not call the grace a fix for the underlying USB protocol error.

## OUT packet sizing and starvation

The pump takes one consumer-availability snapshot per URB and fills all 32
packets from it, so one URB needs 192 frames per playback consumer at
nominal six-frame sizing.

`ignore_plans` (0644, default 0) flattens every packet to six frames and
counts the discarded deviations in `plan_frames_ignored`. It applies
BEFORE the spread below, because flattening a spread URB back to nominal
reintroduces the tail hole. Measured: 14 events of 64 frames in 15 s with
the order reversed.

`even_fill` (0644, default 1) mirrors the vendor builder. When an active
ring holds fewer frames than the plan needs, it spreads that budget over
all 32 packets (budget/32 frames each, plus one on a prefix) and carries
the deficit into the next URB. It applies only when the budget covers
every descriptor, which is the vendor's own condition. Vendor source:
`ysusb_w10_64.sys` FUN_140017efc.

`short_hold` (0644, default 0) repeats the last frame instead of writing
zeros on a shortfall. It changes the content of a hole, never its count,
and hardware trials did not remove the audible artifact it targeted.
Retained as negative evidence.

Read `out_short_frames`, `out_short_events`, `out_short_last`,
`even_fill_urbs` and `even_fill_last_budget` in the OUT block. A healthy
session keeps `out_short_frames` flat while `even_fill_urbs` climbs about
1-2 per second.

## Diagnostics and acceptance

Read `/proc/asound/zg01/usb_stats` before and after each trial.
The ALSA card ID `zg01` avoids reliance on a numeric card index.
Verify the live module srcversion matches the local build before any comparison.
The installed DKMS module can differ from a manually loaded build.

- Inspect per-packet status even when aggregate `urb_errors` is zero.
- `feedback_starved` counts active-IN fallback attempts and the fallback-limit fault.
  It no longer increments for ordinary playback-only free-run completions.
- `driver_xruns` counts the first transport fault per OUT fault latch.
  Later notifications still reach affected streams without recounting the same fault.
- Faulted driver-owned IN hold/assist stops with the failed pair.
  A real capture consumer retains its own ownership unless IN itself fails.
- During an idle keepalive window, both OUT and IN packet counters must advance.
- After expiry with all PCMs closed, both packet counters must stop advancing.
- Confirm capture takeover, suspend/resume, replug, and both playback sinks.
- Use one continuous stereo file. Channel-switching test tones create their own clicks.
- Clean counters do not prove audible onset. Record the first audible marker separately.

The earlier clean-transport startup-discard observations and a startup IN protocol-error
burst are different failure signatures. Do not merge their diagnoses.
