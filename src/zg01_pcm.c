/*
 * Yamaha ZG01 USB Audio Driver - PCM layer
 *
 * One card, three PCM devices:
 *   pcm0 "ZG01 Game"      playback   - consumer of the EP 0x01 chain
 *   pcm1 "ZG01 Voice Out" playback   - consumer of the EP 0x01 chain
 *   pcm2 "ZG01 Voice In"  capture    - owner of the EP 0x81 chain
 *
 * The EP 0x01 URB chain is a single shared resource.  Each packet
 * carries 5 to 7 frames of 40 bytes; the nominal 240-byte packet has
 * slots for BOTH playback consumers (voice L/R at byte
 * offsets 0-7, game L/R at 8-15, 24 pad bytes).  The chain runs while
 * either consumer needs it; the callback mixes each consumer's samples
 * from its own ALSA ring buffer (silence for a consumer that is not
 * running).  The two consumer slots land side by side in each frame;
 * the ZG01 combines both streams at its input.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <sound/info.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>

#include "zg01.h"

/* Startup trials lost early audio despite clean host transport. This
 * optional experiment consumes and DROPS application frames during a
 * silence window while ALSA's clock advances. prime_ms caps the window;
 * valid IN traffic plus a settle delay can release it early. Playback-only
 * starts use a brief IN assist for that transport heuristic, which does
 * not establish audible device readiness. Disabled by default because
 * priming drops application audio; 0 disables priming on fresh starts. */
static unsigned int prime_ms;
module_param(prime_ms, uint, 0644);
MODULE_PARM_DESC(prime_ms, "Silence-priming cap in ms per fresh OUT start (0=off, default 0)");

/* Windows sends a vendor OUT control request (bRequest 0x0b, wValue
 * 0x0060, no data phase) immediately before its per-start interface
 * setup; see captures/2026-09-08_windows-usbmon-verification.md frame
 * 16203. Its purpose is unknown. This replays the exact request once
 * per fresh OUT epoch for a hardware A/B against the device-side
 * discard window. Disabled by default. */
static unsigned int vendor_start_req;
module_param(vendor_start_req, uint, 0644);
MODULE_PARM_DESC(vendor_start_req, "Send Windows start vendor request 0x0b/0x0060 per fresh OUT start (0=off, default 0)");

/* Full Windows start triple: the vendor request above followed by
 * SET_INTERFACE alt 1 on interfaces 1 and 2 (usbmon frames 16203,
 * 16211, 16219). Windows resets the endpoints at EVERY start and
 * hears no discard; the bare request without the reset correlated
 * with discards in hardware trials. The interface reset is forced
 * even when alt 1 is already current — that reset is the point.
 * Interface 2 is only reset when its chain is stopped, so live
 * capture is never killed. */
static unsigned int vendor_start_seq;
module_param(vendor_start_seq, uint, 0644);
MODULE_PARM_DESC(vendor_start_seq, "Full Windows start per fresh OUT start: vendor request + forced SET_INTERFACE alt 1 on both streaming interfaces (0=off, default 0)");

/* Liveness assist: hardware trials (2026-09-08) showed the device
 * discards the first seconds of a fresh OUT stream whenever its IN
 * pipe was idle, and passes audio from the first frame when IN is
 * active around the start — with cold transport, no priming, no EP0
 * requests. The device gates its output path on combined IN+OUT
 * traffic. This runs IN for assist_ms on every fresh playback epoch
 * (via in_assist), long enough to cover the discard window, then
 * drains IN. Playback-only is IN-free again for the rest of the
 * stream, keeping the firmware IN-restart pops out. */
static unsigned int assist_ms;
module_param(assist_ms, uint, 0644);
MODULE_PARM_DESC(assist_ms, "Run IN for N ms at every fresh playback start so the device keeps its output path live (0=off, default 0)");

#define PCM_BUFFER_BYTES_MAX_GAME   (1536 * 32)
#define PCM_BUFFER_BYTES_MIN_GAME   (1536 * 2)
/*
 * The period must stay comfortably above the two-URB (384-frame) hardware
 * read-ahead.  With a 192-frame (one URB) period userspace kept a total
 * delay barely above the read-ahead, so every refill raced the submission
 * and the pump had to pad URB tails with silence (audible periodic gaps).
 * A 384-frame floor leaves the ring room to drain.
 */
#define PCM_PERIOD_BYTES_MIN_GAME   (384 * 8)
#define PCM_PERIOD_BYTES_MAX_GAME   (1536 * 8)

#define PCM_BUFFER_BYTES_MAX_VOICE  (48 * 32 * 64)
#define PCM_PERIOD_BYTES_MIN_VOICE  (48)
#define PCM_PERIOD_BYTES_MAX_VOICE  (48 * 16)

/* Hold the chain warm across short clip gaps: any playback STOP schedules a
 * quiesce instead of stopping, so a START within quiesce_ms adopts the
 * running chain with no new OUT epoch and no prime. Tunable at runtime. */
static unsigned int quiesce_ms = 3000;
module_param(quiesce_ms, uint, 0644);
MODULE_PARM_DESC(quiesce_ms, "Warm-hold window in ms after playback STOP (default 3000)");

/* PipeWire suspends a sink a few seconds after playback stops, which
 * CLOSES the PCM: hw_free fires, cancels the quiesce hold, and stops
 * the chain — so every pause longer than the suspend timeout pays a
 * cold epoch on resume. keepalive_ms extends the warm hold across
 * close/hw_free instead of stopping, letting the OUT chain cycle
 * driver-owned silence buffers with no consumer substream attached.
 * Safe because the pump skips inactive consumers before touching any
 * runtime dma_area, and chain URB buffers are allocated once and never
 * freed outside disconnect. 0 keeps today's stop-at-close behavior. */
static unsigned int keepalive_ms = 600000;
module_param(keepalive_ms, uint, 0644);
MODULE_PARM_DESC(keepalive_ms, "Keep both chains warm across PCM close in ms; the device drops opening audio unless IN isoc keeps streaming (0=off, default 600000)");

static struct zg01_stream *sub_to_stream(struct snd_pcm_substream *substream)
{
    return substream->pcm->private_data;
}

static const char *stream_name(const struct zg01_stream *s)
{
    return s->direction == SNDRV_PCM_STREAM_CAPTURE ? "Voice In" :
           (s->pcm_device == ZG01_PCM_GAME ? "Game Out" : "Voice Out");
}

/* Snapshot for consumer demand; submission decisions must hold dev->lock. */
static bool zg01_chain_active(struct zg01_chain *c)
{
    enum zg01_chain_state state = READ_ONCE(c->state);

    return state == ZG01_CHAIN_STARTING || state == ZG01_CHAIN_RUNNING;
}

/* Is any consumer of this chain currently running (state_mutex)? */
static bool chain_consumers_running(struct zg01_dev *dev, struct zg01_chain *c)
{
    if (c == &dev->out_chain)
        return dev->streams[ZG01_GAME].running ||
               dev->streams[ZG01_VOICE_OUT].running ||
               READ_ONCE(dev->out_silence) ||
               READ_ONCE(dev->out_hold);
    /* EXPERIMENT: IN runs only for real capture, plus the brief
     * priming-assist window (in_assist) that clocks the prime
     * release in playback-only mode. The device firmware
     * periodically restarts its IN endpoint mid-stream (zero-length
     * packet + header counter reset, ~1/20s), and each restart pops
     * the shared output. Fixed-cadence OUT no longer needs IN pacing,
     * so keep IN off during playback-only to avoid triggering it. */
    return dev->streams[ZG01_VOICE_IN].running ||
           READ_ONCE(dev->in_assist) ||
           READ_ONCE(dev->in_hold);
}

/* Warm-hold decision: normal STOP holds, drain-final STOP stops at once. */
static bool zg01_hold_warm(bool draining)
{
    return !draining;
}

/* Close/hw_free hold decision: with keepalive armed, playback close
 * leaves the OUT chain cycling driver-owned silence instead of
 * stopping, so a PipeWire suspend/resume cycle does not pay a cold
 * epoch. Capture close never holds (IN stays capture-only).
 * Hardware trials (2026-09-08): only real IN isoc traffic keeps the
 * device output path live — OUT silence alone, EP0 requests, control
 * heartbeats, and bulk polls all failed to. So the hold now also
 * keeps the IN chain cycling (driver-owned, no capture substream):
 * the device stays in its IN+OUT live state through the window. */
static bool zg01_hold_across_close(struct zg01_dev *dev)
{
    return keepalive_ms &&
           !dev->streams[ZG01_VOICE_IN].running &&
           !dev->streams[ZG01_VOICE_IN].opened;
}

static int zg01_chain_start(struct zg01_dev *dev, struct zg01_chain *c);

/* Arm the both-chains keepalive hold. Caller must NOT hold
 * state_mutex: zg01_chain_start takes it. Drain-final STOP has
 * already stopped OUT by the time hw_free/close run, so this
 * RESTARTS OUT as a driver-owned silence chain (out_hold) and then
 * starts IN (in_hold). IN plans keep the OUT pump fed; together
 * they hold the device in its live IN+OUT state. */
static void zg01_keepalive_arm(struct zg01_dev *dev)
{
    unsigned long flags;
    bool start_out, start_in;
    int ret;

    if (!keepalive_ms)
        return;

    spin_lock_irqsave(&dev->lock, flags);
    start_out = READ_ONCE(dev->out_chain.state) == ZG01_CHAIN_STOPPED;
    start_in = READ_ONCE(dev->in_chain.state) == ZG01_CHAIN_STOPPED &&
               !dev->streams[ZG01_VOICE_IN].running &&
               !dev->streams[ZG01_VOICE_IN].opened;
    if (start_out)
        WRITE_ONCE(dev->out_hold, true);
    if (start_in)
        WRITE_ONCE(dev->in_hold, true);
    spin_unlock_irqrestore(&dev->lock, flags);

    if (start_out) {
        ret = zg01_chain_start(dev, &dev->out_chain);
        if (ret && ret != -ECANCELED) {
            spin_lock_irqsave(&dev->lock, flags);
            WRITE_ONCE(dev->out_hold, false);
            spin_unlock_irqrestore(&dev->lock, flags);
            dev_warn(&dev->udev->dev, "keepalive OUT start: %d\n", ret);
            return;
        }
    }
    if (start_in) {
        ret = zg01_chain_start(dev, &dev->in_chain);
        if (ret && ret != -ECANCELED) {
            spin_lock_irqsave(&dev->lock, flags);
            WRITE_ONCE(dev->in_hold, false);
            spin_unlock_irqrestore(&dev->lock, flags);
            dev_warn(&dev->udev->dev, "keepalive IN start: %d\n", ret);
        }
    }
}

static void zg01_chain_stop(struct zg01_dev *dev, struct zg01_chain *c);
static void zg01_feedback_pump(struct zg01_dev *dev);
static void zg01_feedback_xrun(struct zg01_dev *dev);
static void zg01_feedback_xrun_all(struct zg01_dev *dev);

/* ================================================================== */
/* Deferred work handlers                                              */
/* ================================================================== */

void zg01_period_work_fn(struct work_struct *work)
{
    struct zg01_stream *s =
        container_of(work, struct zg01_stream, period_work);
    struct zg01_dev *dev = s->dev;
    struct snd_pcm_substream *sub;
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    sub = s->substream;
    spin_unlock_irqrestore(&dev->lock, flags);

    if (sub && !atomic_read(&dev->disconnecting))
        snd_pcm_period_elapsed(sub);
}

/* snd_pcm_stop_xrun() takes the PCM stream lock, which SLEEPS
 * for a nonatomic PCM.  Never call it from the URB callback. */
void zg01_xrun_work_fn(struct work_struct *work)
{
    struct zg01_stream *s = container_of(work, struct zg01_stream, xrun_work);
    struct zg01_dev *dev = s->dev;
    struct snd_pcm_substream *sub;
    unsigned long flags, pcm_flags;
    unsigned int generation;
    bool stop;

    spin_lock_irqsave(&dev->lock, flags);
    sub = s->substream;
    generation = s->xrun_generation;
    spin_unlock_irqrestore(&dev->lock, flags);
    if (!sub || !generation || atomic_read(&dev->disconnecting))
        return;

    /* Serialize the generation check with ALSA prepare/trigger. Never hold
     * dev->lock while taking the (nonatomic, sleeping) PCM stream lock. */
    snd_pcm_stream_lock_irqsave(sub, pcm_flags);
    spin_lock_irqsave(&dev->lock, flags);
    stop = s->substream == sub && s->enabled && s->generation == generation;
    spin_unlock_irqrestore(&dev->lock, flags);
    if (stop && snd_pcm_running(sub))
        snd_pcm_stop(sub, SNDRV_PCM_STATE_XRUN);
    snd_pcm_stream_unlock_irqrestore(sub, pcm_flags);
}

void zg01_chain_cleanup_fn(struct work_struct *work)
{
    struct zg01_chain *c = container_of(work, struct zg01_chain, cleanup_work);
    struct zg01_dev *dev = c->dev;
    unsigned long flags;
    int i;

    /* DRAINING blocks all submissions, including the other endpoint's
     * OUT pump. Join this chain's callbacks; the other chain may run. */
    for (i = 0; i < MAX_URBS; i++)
        if (c->urbs[i])
            usb_kill_urb(c->urbs[i]);

    mutex_lock(&dev->state_mutex);
    spin_lock_irqsave(&dev->lock, flags);
    WARN_ON_ONCE(c->state != ZG01_CHAIN_DRAINING);
    WARN_ON_ONCE(atomic_read(&c->inflight));
    WRITE_ONCE(c->state, ZG01_CHAIN_STOPPED);
    spin_unlock_irqrestore(&dev->lock, flags);
    mutex_unlock(&dev->state_mutex);
}

/*
 * A rapid STOP/START burst that ends on a STOP leaves the chain
 * streaming silence forever.  Schedule a quiesce; a later START cancels
 * it asynchronously (the quiesce then no-ops on consumers-running).
 */
void zg01_chain_quiesce_fn(struct work_struct *work)
{
    struct zg01_chain *c = container_of(work, struct zg01_chain, quiesce_work.work);
    struct zg01_dev *dev = c->dev;
    unsigned long flags;

    mutex_lock(&dev->state_mutex);
    /* Keepalive window elapsed: drop the driver-owned hold flags so
     * the consumers-running checks below stop both chains. Capture
     * or playback still running keeps its chain regardless. */
    spin_lock_irqsave(&dev->lock, flags);
    WRITE_ONCE(dev->in_hold, false);
    WRITE_ONCE(dev->out_hold, false);
    spin_unlock_irqrestore(&dev->lock, flags);
    if (!chain_consumers_running(dev, c))
        zg01_chain_stop(dev, c);
    if (!chain_consumers_running(dev, &dev->in_chain))
        zg01_chain_stop(dev, &dev->in_chain);
    mutex_unlock(&dev->state_mutex);
}

/* Quiesce runs for either chain: when the keepalive window ends with
 * no consumers, release the driver-owned IN hold before the checks
 * above so both chains stop together. */

/* ================================================================== */
/* Chain primitives (state_mutex side)                                 */
/* ================================================================== */

static void zg01_iso_out(struct urb *urb);
static void zg01_iso_in(struct urb *urb);

static int zg01_chain_alloc(struct zg01_dev *dev, struct zg01_chain *c,
                            unsigned int endpoint, unsigned int interface_num,
                            unsigned int iso_pkts, unsigned int iso_pkt_size,
                            bool playback)
{
    int i, k;

    if (c->allocated)
        return 0;

    c->dev = dev;
    c->endpoint = endpoint;
    c->interface_num = interface_num;
    c->iso_pkts = iso_pkts;
    c->iso_pkt_size = iso_pkt_size;

    for (i = 0; i < MAX_URBS; i++) {
        c->urbs[i] = usb_alloc_urb(iso_pkts, GFP_KERNEL);
        c->bufs[i] = kmalloc(iso_pkts * iso_pkt_size, GFP_KERNEL);
        if (!c->urbs[i] || !c->bufs[i]) {
            usb_free_urb(c->urbs[i]);
            kfree(c->bufs[i]);
            c->urbs[i] = NULL;
            c->bufs[i] = NULL;
            goto fail;
        }
        if (playback)
            memset(c->bufs[i], 0, iso_pkts * iso_pkt_size);
        c->urbs[i]->dev = dev->udev;
        c->urbs[i]->pipe = (endpoint & USB_DIR_IN)
            ? usb_rcvisocpipe(dev->udev, endpoint & 0x0F)
            : usb_sndisocpipe(dev->udev, endpoint & 0x0F);
        c->urbs[i]->transfer_buffer = c->bufs[i];
        c->urbs[i]->transfer_buffer_length = iso_pkts * iso_pkt_size;
        c->urbs[i]->complete = playback ? zg01_iso_out : zg01_iso_in;
        c->urbs[i]->context = c;
        c->urbs[i]->interval = 1;
        c->urbs[i]->start_frame = -1;
        c->urbs[i]->number_of_packets = iso_pkts;
        c->urbs[i]->transfer_flags = URB_ISO_ASAP;
        for (k = 0; k < iso_pkts; k++) {
            c->urbs[i]->iso_frame_desc[k].offset = k * iso_pkt_size;
            c->urbs[i]->iso_frame_desc[k].length = iso_pkt_size;
        }
    }

    c->allocated = true;
    return 0;

fail:
    for (k = 0; k < i; k++) {
        usb_free_urb(c->urbs[k]);
        kfree(c->bufs[k]);
        c->urbs[k] = NULL;
        c->bufs[k] = NULL;
    }
    return -ENOMEM;
}

/* All stops and faults enter here with dev->lock held, including callbacks.
 * Publish the submission barrier before queuing the sole drain owner.
 * No unlink is needed here: the worker joins every URB with usb_kill_urb(). */
static void zg01_chain_drain_locked(struct zg01_chain *c)
{
    lockdep_assert_held(&c->dev->lock);

    if (!zg01_chain_active(c))
        return;
    WRITE_ONCE(c->state, ZG01_CHAIN_DRAINING);
    queue_work(zg01_cleanup_wq, &c->cleanup_work);
}

/* state_mutex serializes consumer changes; dev->lock closes callback races. */
static void zg01_chain_stop(struct zg01_dev *dev, struct zg01_chain *c)
{
    unsigned long flags;

    lockdep_assert_held(&dev->state_mutex);
    spin_lock_irqsave(&dev->lock, flags);
    zg01_chain_drain_locked(c);
    spin_unlock_irqrestore(&dev->lock, flags);
}

/* Return with state_mutex held. The caller must still check state under
 * dev->lock: a callback may request a drain after this snapshot. */
static int zg01_chain_wait_cleanup(struct zg01_dev *dev, struct zg01_chain *c)
{
    lockdep_assert_held(&dev->state_mutex);
    while (!atomic_read(&dev->disconnecting) &&
           READ_ONCE(c->state) == ZG01_CHAIN_DRAINING) {
        mutex_unlock(&dev->state_mutex);
        flush_work(&c->cleanup_work);
        mutex_lock(&dev->state_mutex);
    }
    return atomic_read(&dev->disconnecting) ? -ENODEV : 0;
}

/* Called without state_mutex. Adopt RUNNING, or initialize only STOPPED.
 * RUNNING OUT may wait for feedback with zero submissions. */
static int zg01_chain_start(struct zg01_dev *dev, struct zg01_chain *c)
{
    unsigned long flags;
    int i, ret;

    /* No _sync: quiesce also takes state_mutex. */
    cancel_delayed_work(&c->quiesce_work);
    mutex_lock(&dev->state_mutex);
retry:
    ret = zg01_chain_wait_cleanup(dev, c);
    if (ret)
        goto unlock;
    if (!c->allocated) {
        ret = -ENOMEM;
        goto unlock;
    }
    if (!chain_consumers_running(dev, c)) {
        ret = -ECANCELED;
        goto unlock;
    }

    spin_lock_irqsave(&dev->lock, flags);
    if (c->state == ZG01_CHAIN_DRAINING) {
        spin_unlock_irqrestore(&dev->lock, flags);
        goto retry;
    }
    if (c->state == ZG01_CHAIN_RUNNING) {
        spin_unlock_irqrestore(&dev->lock, flags);
        goto unlock;
    }
    /* state_mutex excludes another STARTING owner. STOPPED comes from
     * initial allocation or a worker that has joined every callback. */
    if (WARN_ON_ONCE(c->state != ZG01_CHAIN_STOPPED ||
                     atomic_read(&c->inflight))) {
        ret = -EIO;
        spin_unlock_irqrestore(&dev->lock, flags);
        goto unlock;
    }
    spin_unlock_irqrestore(&dev->lock, flags);

    /* EXPERIMENT: Windows precedes every start with a vendor OUT
     * request 0x0b/0x0060 (no data phase). Send it only on a fresh
     * OUT epoch, outside dev->lock because usb_control_msg sleeps.
     * vendor_start_seq subsumes vendor_start_req: the full Windows
     * triple additionally forces SET_INTERFACE alt 1 on both
     * streaming interfaces (skipping interface 2 while its chain is
     * live). Failures are logged, not fatal. */
    if (c == &dev->out_chain &&
        (READ_ONCE(vendor_start_req) || READ_ONCE(vendor_start_seq))) {
        ret = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
                              0x0b, USB_DIR_OUT | USB_TYPE_VENDOR |
                              USB_RECIP_DEVICE,
                              0x0060, 0x0000, NULL, 0, 1000);
        if (ret < 0)
            dev_warn(&dev->udev->dev,
                     "vendor start request failed: %d\n", ret);
        if (READ_ONCE(vendor_start_seq)) {
            ret = usb_set_interface(dev->udev, 1, 1);
            if (ret < 0)
                dev_warn(&dev->udev->dev,
                         "start reset interface 1 failed: %d\n", ret);
            if (READ_ONCE(dev->in_chain.state) == ZG01_CHAIN_STOPPED) {
                ret = usb_set_interface(dev->udev, 2, 1);
                if (ret < 0)
                    dev_warn(&dev->udev->dev,
                             "start reset interface 2 failed: %d\n", ret);
            }
        }
    }

    spin_lock_irqsave(&dev->lock, flags);
    if (c == &dev->out_chain) {
        zg01_feedback_reset(&dev->feedback);
        dev->have_last_plan = false;
        dev->feedback_started = false;
        dev->feedback_fault = false;
        dev->feedback_gap_urbs = 0;
        dev->feedback_startup_urbs = 0;
        memset(c->completed_frames, 0, sizeof(c->completed_frames));
        /* New OUT epoch: re-arm the first-event latches so every
         * transport start reports its own startup timing. Live
         * adoption (RUNNING above) keeps the current epoch. */
        c->stats.first_nonzero_copy_ns = 0;
        c->stats.first_nonzero_copy_frame = 0;
        c->stats.first_nonzero_submit_ns = 0;
        c->stats.first_out_completion_ns = 0;
        c->stats.start_epoch++;
        /* Device discard-window priming: send pure silence for the
         * first prime_ms of every fresh OUT epoch, consuming and
         * dropping the application's frames so ALSA's clock runs.
         * Armed only when a playback consumer runs: a capture-owned
         * silence epoch has no application frames to protect, and a
         * later playback adoption must not pay a stale window. */
        c->dev->prime_deadline_ns = (prime_ms &&
            (dev->streams[ZG01_GAME].running ||
             dev->streams[ZG01_VOICE_OUT].running)) ?
            ktime_get_ns() + (u64)prime_ms * NSEC_PER_MSEC : 0;
        /* Only this drained, fresh OUT initialization may arm assist.
         * RUNNING adoption returns above and preserves the current epoch. */
        dev->in_assist = dev->prime_deadline_ns &&
                         !dev->streams[ZG01_VOICE_IN].running;
        /* Liveness assist (assist_ms): cover the device discard window
         * with real IN traffic even when not priming. Same arming
         * rules as prime assist: fresh playback epoch only, never
         * when capture owns the IN chain. */
        if (assist_ms && !dev->prime_deadline_ns &&
            (dev->streams[ZG01_GAME].running ||
             dev->streams[ZG01_VOICE_OUT].running) &&
            !dev->streams[ZG01_VOICE_IN].running) {
            dev->assist_deadline_ns = ktime_get_ns() +
                (u64)assist_ms * NSEC_PER_MSEC;
            dev->in_assist = true;
        } else {
            dev->assist_deadline_ns = 0;
        }
        c->dev->prime_ready_ns = 0;
        c->dev->prime_released_frame = 0;
        c->dev->prime_in_first_ns = 0;
        for (i = 0; i < MAX_URBS; i++) {
            memset(c->bufs[i], 0, c->iso_pkts * c->iso_pkt_size);
            zg01_feedback_pending(&dev->feedback, i);
        }
        WRITE_ONCE(c->state, ZG01_CHAIN_RUNNING);
        spin_unlock_irqrestore(&dev->lock, flags);
        goto unlock;
    }
    /* A fresh IN chain starts a new liveness window. Preserve OUT's
     * pending IDs and inflight audio, but discard stale IN observations. */
    dev->feedback.plan_head = 0;
    dev->feedback.plans = 0;
    dev->have_last_plan = false;
    dev->feedback_started = false;
    dev->feedback_gap_urbs = 0;
    dev->feedback_startup_urbs = 0;
    WRITE_ONCE(c->state, ZG01_CHAIN_STARTING);
    spin_unlock_irqrestore(&dev->lock, flags);

    /* Gate each initial submission against callback faults. Account only
     * URBs actually submitted, not a reservation for the whole batch. */
    for (i = 0; i < MAX_URBS; i++) {
        spin_lock_irqsave(&dev->lock, flags);
        if (atomic_read(&dev->disconnecting) ||
            c->state != ZG01_CHAIN_STARTING) {
            ret = -EPIPE;
        } else {
            atomic_inc(&c->inflight);
            ret = usb_submit_urb(c->urbs[i], GFP_ATOMIC);
            if (ret)
                atomic_dec(&c->inflight);
        }
        if (ret)
            zg01_chain_drain_locked(c);
        spin_unlock_irqrestore(&dev->lock, flags);
        if (ret)
            goto unlock;
    }
    spin_lock_irqsave(&dev->lock, flags);
    if (c->state == ZG01_CHAIN_STARTING)
        WRITE_ONCE(c->state, ZG01_CHAIN_RUNNING);
    else
        ret = -EPIPE;
    spin_unlock_irqrestore(&dev->lock, flags);
unlock:
    mutex_unlock(&dev->state_mutex);
    return ret;
}

void zg01_stop_all_chains(struct zg01_dev *dev)
{
    /* cancel_sync must run OUTSIDE state_mutex: the quiesce handler
     * takes it (deadlock otherwise). */
    cancel_delayed_work_sync(&dev->out_chain.quiesce_work);
    cancel_delayed_work_sync(&dev->in_chain.quiesce_work);
    mutex_lock(&dev->state_mutex);
    zg01_chain_stop(dev, &dev->out_chain);
    zg01_chain_stop(dev, &dev->in_chain);
    mutex_unlock(&dev->state_mutex);
}

void zg01_drain_all_chains(struct zg01_dev *dev)
{
    flush_work(&dev->out_chain.cleanup_work);
    flush_work(&dev->in_chain.cleanup_work);
}

/* ================================================================== */
/* Rate / vendor "Magic Sequence"                                      */
/* ================================================================== */

/*
 * Resets BOTH interfaces to alt 0 — kills every live URB.  Callers must
 * hold state_mutex and have verified no chain is streaming.
 */
int zg01_set_rate(struct zg01_dev *dev, int rate)
{
    unsigned char *data;
    unsigned char *large_data;
    int ret = 0;
    int fatal = 0;
    const char *fatal_stage = NULL;
    const char *stage = "allocation";

    if (!dev || !dev->udev)
        return -ENODEV;

    data = kmalloc(4, GFP_KERNEL);
    large_data = kmalloc(72, GFP_KERNEL);
    if (!data || !large_data) {
        kfree(data);
        kfree(large_data);
        return -ENOMEM;
    }

    dev_dbg(&dev->udev->dev, "set_rate %d\n", rate);

    /* The firmware STALLs vendor EP0 reads unpredictably (three
     * different requests across two sessions, usbmon verified) and
     * aborted sequences make it worse: leaving the handshake half-done
     * (interfaces at alt 0, commit writes unsent) makes the next
     * attempt stall more. So send the COMPLETE legacy sequence every
     * time, never abort mid-sequence, log advisory results, restore
     * alt 1 unconditionally, and report the first load-bearing failure
     * (interface change or clock) at the end. Vendor read payloads are
     * unused downstream; the legacy driver ignored all return codes. */
    stage = "vendor 0x07";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x07, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x0000, 0x0000, large_data, 3, 1000);
    if (ret != 3)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x07: %d (continuing)\n", ret);
    stage = "vendor 0x04";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x04, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x0000, 0x0000, large_data, 1, 1000);
    if (ret != 1)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x04: %d (continuing)\n", ret);
    stage = "vendor 0x0a";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x0a, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x0000, 0x0000, large_data, 4, 1000);
    if (ret != 4)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x0a: %d (continuing)\n", ret);
    stage = "vendor 0x0c/0x8000";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x0c, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x8000, 0x0000, large_data, 72, 1000);
    if (ret != 72)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x0c/0x8000: %d (continuing)\n", ret);
    stage = "vendor 0x0c/0x0000";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x0c, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x0000, 0x0000, large_data, 72, 1000);
    if (ret != 72)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x0c/0x0000: %d (continuing)\n", ret);

    /* 2. Interfaces to alt 0 */
    stage = "interface 1 alt 0";
    ret = usb_set_interface(dev->udev, 1, 0);
    if (ret < 0 && !fatal) {
        fatal = ret;
        fatal_stage = stage;
    }
    stage = "interface 2 alt 0";
    ret = usb_set_interface(dev->udev, 2, 0);
    if (ret < 0 && !fatal) {
        fatal = ret;
        fatal_stage = stage;
    }

    /* 3. UAC2 SET_CUR on clock source 1, verify with GET_CUR */
    data[0] = rate & 0xff;
    data[1] = (rate >> 8) & 0xff;
    data[2] = (rate >> 16) & 0xff;
    data[3] = (rate >> 24) & 0xff;
    {
        int attempt;

        for (attempt = 1; attempt <= 3; attempt++) {
            stage = "clock SET_CUR";
            ret = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
                                  0x01, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                  0x0100, 0x0100, data, 4, 1000);
            if (ret != 4) {
                ret = ret < 0 ? ret : -EIO;
                goto retry_rate;
            }
            stage = "clock GET_CUR";
            ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                                  0x01, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                                  0x0100, 0x0100, large_data, 4, 1000);
            if (ret == 4) {
                unsigned int ret_rate = (u32)large_data[0] |
                                        ((u32)large_data[1] << 8) |
                                        ((u32)large_data[2] << 16) |
                                        ((u32)large_data[3] << 24);

                /* Every PCM advertises 48 kHz. A different device rate
                 * cannot satisfy that contract without conversion. */
                ret = ret_rate == (unsigned int)rate ? 0 : -ERANGE;
                if (!ret)
                    break;
            } else {
                ret = ret < 0 ? ret : -EIO;
            }
retry_rate:
            if (attempt < 3)
                msleep(150);
        }
        if (ret && !fatal) {
            fatal = ret;
            fatal_stage = stage;
        }
    }

    /* 4. Commit handshake. Payloads are unused and these stall like
     * the discovery reads; log-and-continue so the device still sees
     * the complete sequence. */
    stage = "vendor 0x02/0x0002";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x02, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x0002, 0x0000, large_data, 1, 1000);
    if (ret != 1)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x02/0x0002: %d (continuing)\n", ret);
    stage = "vendor 0x02/0x0001";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x02, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x0001, 0x0000, large_data, 1, 1000);
    if (ret != 1)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x02/0x0001: %d (continuing)\n", ret);
    stage = "vendor 0x08";
    ret = usb_control_msg(dev->udev, usb_rcvctrlpipe(dev->udev, 0),
                    0x08, USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
                    0x0000, 0x0000, large_data, 1, 1000);
    if (ret != 1)
        dev_warn_ratelimited(&dev->udev->dev, "vendor read 0x08: %d (continuing)\n", ret);
    stage = "vendor 0x00";
    ret = usb_control_msg(dev->udev, usb_sndctrlpipe(dev->udev, 0),
                    0x00, USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
                    0x0000, 0x0000, NULL, 0, 1000);
    if (ret != 0)
        dev_warn_ratelimited(&dev->udev->dev, "vendor write 0x00: %d (continuing)\n", ret);

    /* 5. Streaming interfaces back to alt 1 — unconditional, even
     * after an earlier fatal step, so a failed attempt leaves the
     * device in the streaming configuration for the next try. */
    stage = "interface 1 alt 1";
    ret = usb_set_interface(dev->udev, 1, 1);
    if (ret < 0 && !fatal) {
        fatal = ret;
        fatal_stage = stage;
    }
    stage = "interface 2 alt 1";
    ret = usb_set_interface(dev->udev, 2, 1);
    if (ret < 0 && !fatal) {
        fatal = ret;
        fatal_stage = stage;
    }

    msleep(200);

    ret = fatal;
    if (ret < 0)
        dev_warn_ratelimited(&dev->udev->dev, "initialization failed at %s: %d\n",
                             fatal_stage ? fatal_stage : "allocation", ret);
    kfree(large_data);
    kfree(data);
    return ret;
}

/* ================================================================== */
/* ISO callbacks                                                       */
/* ================================================================== */

/* Account every non-cancelled packet, before any PCM length/state filter.
 * Caller holds dev->lock. No printk or allocation in the completion path. */
static void zg01_usb_stats_account(struct zg01_usb_stats *s,
                                   const struct urb *urb, bool capture, u64 now)
{
    int i;

    s->completions++;
    if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
        urb->status == -ESHUTDOWN) {
        s->cancelled++;
        return;
    }
    if (urb->status) {
        s->urb_errors++;
        s->last_error_ns = now;
    }

    for (i = 0; i < urb->number_of_packets; i++) {
        const struct usb_iso_packet_descriptor *p = &urb->iso_frame_desc[i];

        s->packets++;
        if (p->status <= 0 && p->status > -128)
            s->packet_status[-p->status]++;
        else
            s->unknown_status++;
        if (p->status) {
            s->last_error_ns = now;
            continue;
        }
        if (capture) {
            if (p->actual_length <= ISO_PKT_SIZE_IN)
                s->in_length[p->actual_length]++;
            else
                s->in_length_overflow++;
        } else if (p->actual_length != p->length) {
            s->out_length_mismatch++;
            s->last_error_ns = now;
        } else if (p->length >= 200 && p->length <= 280 && !(p->length % 40)) {
            s->out_frames[p->length / 40]++;
        }
    }
}

/* Retain startup and the packet on either side of a length/status change.
 * Freeze records at capacity; continue counting omitted selected records. */
static void zg01_in_trace_append(struct zg01_in_trace *t,
                                 const struct zg01_in_record *r)
{
    /* Ring of the most recent records: once full, each append replaces
     * the oldest slot.  'omitted' counts displaced records so a reader
     * still sees that history was lost. */
    if (t->count == ZG01_TRACE_RECORDS) {
        t->records[t->next] = *r;
        t->next = (t->next + 1) % ZG01_TRACE_RECORDS;
        t->omitted++;
        return;
    }
    t->records[t->next] = *r;
    t->next = (t->next + 1) % ZG01_TRACE_RECORDS;
    t->count++;
}

static void zg01_in_trace_account(struct zg01_in_trace *t,
                                  const struct urb *urb, u64 now)
{
    int i;

    if (urb->status) {
        t->have_previous = false;
        return;
    }
    for (i = 0; i < urb->number_of_packets; i++) {
        const struct usb_iso_packet_descriptor *p = &urb->iso_frame_desc[i];
        struct zg01_in_record r = {
            .seq = ++t->packets,
            .completion_ns = now,
            .length = p->actual_length,
            .status = p->status,
        };
        bool changed;

        /* Never read failed, short, or out-of-bounds packet data. */
        if (!p->status && p->actual_length >= sizeof(r.header) &&
            p->actual_length <= p->length && urb->transfer_buffer &&
            urb->transfer_buffer_length >= 0 &&
            p->offset <= (unsigned int)urb->transfer_buffer_length &&
            p->actual_length <= (unsigned int)urb->transfer_buffer_length - p->offset) {
            memcpy(r.header, (u8 *)urb->transfer_buffer + p->offset,
                   sizeof(r.header));
            r.header_valid = true;
        }
        changed = t->have_previous &&
                  (r.length != t->previous.length ||
                   r.status != t->previous.status);
        if (changed)
            zg01_in_trace_append(t, &t->previous);
        if (!t->have_previous || r.seq <= 4 || changed || r.status ||
            r.length != 108)
            zg01_in_trace_append(t, &r);
        t->previous = r;
        t->have_previous = true;
    }
}

static void zg01_in_trace_read(struct snd_info_entry *entry,
                               struct snd_info_buffer *buffer)
{
    struct zg01_dev *dev = entry->private_data;
    struct zg01_in_trace *t;
    unsigned long flags;
    unsigned int i;

    t = kmalloc(sizeof(*t), GFP_KERNEL);
    if (!t) {
        snd_iprintf(buffer, "snapshot allocation failed\n");
        return;
    }
    spin_lock_irqsave(&dev->lock, flags);
    *t = dev->in_trace;
    spin_unlock_irqrestore(&dev->lock, flags);

    snd_iprintf(buffer, "zg01_in_trace_v2\npackets %llu\nretained %u\nomitted %llu\n",
                t->packets, t->count, t->omitted);
    snd_iprintf(buffer, "# seq completion_ns length status header_hex\n");
    /* Oldest first: full ring starts at t->next, partial at 0. */
    for (i = 0; i < t->count; i++) {
        const struct zg01_in_record *r =
            &t->records[(t->next + ZG01_TRACE_RECORDS - t->count + i) %
                        ZG01_TRACE_RECORDS];

        snd_iprintf(buffer, "%llu %llu %u %d ", r->seq,
                    r->completion_ns, r->length, r->status);
        if (r->header_valid)
            snd_iprintf(buffer, "%8phN\n", r->header);
        else
            snd_iprintf(buffer, "-\n");
    }
    kfree(t);
}

static void zg01_usb_stats_print(struct snd_info_buffer *buffer,
                                 const struct zg01_usb_stats *s, bool capture)
{
    int i;

    snd_iprintf(buffer, "%s\n", capture ? "IN 0x81" : "OUT 0x01");
    snd_iprintf(buffer, "completions %llu\ncancelled %llu\nurb_errors %llu\n",
                s->completions, s->cancelled, s->urb_errors);
    snd_iprintf(buffer, "packets %llu\nunknown_status %llu\nlast_error_ns %llu\n",
                s->packets, s->unknown_status, s->last_error_ns);
    snd_iprintf(buffer, "first_nonzero_copy_ns %llu\n"
                "first_nonzero_copy_frame %llu\n"
                "first_nonzero_submit_ns %llu\n"
                "first_out_completion_ns %llu\n"
                "start_epoch %llu\n",
                s->first_nonzero_copy_ns, s->first_nonzero_copy_frame,
                s->first_nonzero_submit_ns, s->first_out_completion_ns,
                s->start_epoch);
    snd_iprintf(buffer, "feedback_valid %llu\nfeedback_invalid %llu\n"
                "feedback_starved %llu\nfeedback_overflow %llu\n"
                "feedback_submit_errors %llu\nplayback_waits %llu\n"
                "playback_defer %llu\nsilence_frames %llu\n"
                "driver_xruns %llu\n",
                s->feedback_valid, s->feedback_invalid, s->feedback_starved,
                s->feedback_overflow, s->feedback_submit_errors, s->playback_waits,
                s->playback_defer, s->silence_frames,
                s->driver_xruns);
    for (i = 0; i < ARRAY_SIZE(s->packet_status); i++)
        if (s->packet_status[i])
            snd_iprintf(buffer, "packet_status %d %llu\n", -i,
                        s->packet_status[i]);
    if (capture) {
        for (i = 0; i < ARRAY_SIZE(s->in_length); i++)
            if (s->in_length[i])
                snd_iprintf(buffer, "in_length %d %llu\n", i, s->in_length[i]);
        snd_iprintf(buffer, "in_length_overflow %llu\n", s->in_length_overflow);
    } else {
        snd_iprintf(buffer, "out_length_mismatch %llu\n", s->out_length_mismatch);
        for (i = 5; i <= 7; i++)
            snd_iprintf(buffer, "out_frames %d %llu\n", i, s->out_frames[i]);
    }
}

static void zg01_usb_stats_read(struct snd_info_entry *entry,
                                struct snd_info_buffer *buffer)
{
    struct zg01_dev *dev = entry->private_data;
    struct zg01_usb_stats *snapshot;
    unsigned long flags;
    u64 now;
    u64 prime_ready_ns, prime_released_frame, prime_in_first_ns;

    /* Keep the snapshots off the kernel stack; format after releasing lock. */
    snapshot = kmalloc_array(2, sizeof(*snapshot), GFP_KERNEL);
    if (!snapshot) {
        snd_iprintf(buffer, "snapshot allocation failed\n");
        return;
    }
    spin_lock_irqsave(&dev->lock, flags);
    snapshot[0] = dev->out_chain.stats;
    snapshot[1] = dev->in_chain.stats;
    prime_ready_ns = dev->prime_ready_ns;
    prime_released_frame = dev->prime_released_frame;
    prime_in_first_ns = dev->prime_in_first_ns;
    now = ktime_get_ns();
    spin_unlock_irqrestore(&dev->lock, flags);

    snd_iprintf(buffer, "zg01_usb_stats_v1\nsnapshot_ns %llu\n", now);
    zg01_usb_stats_print(buffer, &snapshot[0], false);
    snd_iprintf(buffer, "prime_ready_ns %llu\nprime_released_frame %llu\n"
                "prime_in_first_ns %llu\n",
                prime_ready_ns, prime_released_frame, prime_in_first_ns);
    zg01_usb_stats_print(buffer, &snapshot[1], true);
    kfree(snapshot);
}

/* Called with dev->lock held. False drops this URB's accounting claim;
 * only usb_kill_urb(), not that decrement, joins the callback's return. */
static bool chain_resubmit(struct zg01_chain *c, struct urb *urb)
{
    struct zg01_dev *dev = c->dev;
    int i, ret;
    bool terminal = urb->status == -ESHUTDOWN ||
                    urb->status == -ENOENT ||
                    urb->status == -ECONNRESET;

    if (atomic_read(&dev->disconnecting) || !zg01_chain_active(c) ||
        terminal) {
        if (terminal && !atomic_read(&dev->disconnecting) &&
            zg01_chain_active(c)) {
            zg01_chain_drain_locked(c);
            /* IN endpoint death (URB kill/shutdown) takes capture down
             * with playback; the pacing source is gone for both. */
            if (c == &dev->in_chain)
                zg01_feedback_xrun_all(dev);
            else
                zg01_feedback_xrun(dev);
        }
        atomic_dec(&c->inflight);
        return false;
    }

    for (i = 0; i < urb->number_of_packets; i++) {
        urb->iso_frame_desc[i].status = 0;
        urb->iso_frame_desc[i].actual_length = 0;
    }

    ret = usb_submit_urb(urb, GFP_ATOMIC);
    if (ret < 0) {
        dev_warn(&dev->udev->dev, "resubmit failed on EP 0x%02x: %d\n",
                 c->endpoint, ret);
        atomic_dec(&c->inflight);
        /* Surface the dead chain to ALSA as an xrun instead of
         * clicking silently forever.  Queuing work from softirq is
         * safe; the handlers check substream state themselves. */
        c->stats.feedback_submit_errors++;
        zg01_chain_drain_locked(c);
        if (c == &dev->in_chain)
            zg01_feedback_xrun_all(dev);
        else
            zg01_feedback_xrun(dev);
        return false;
    }
    return true;
}

/* Snapshot and all ring access are serialized against STOP/hw_free. */
struct out_consumer {
    struct zg01_stream *s;
    struct snd_pcm_runtime *rt;
    unsigned int available;
    bool active;
};

static void snap_consumer(struct zg01_dev *dev, enum zg01_stream_id id,
                          struct out_consumer *oc)
{
    struct zg01_stream *s = &dev->streams[id];
    struct snd_pcm_substream *sub = s->substream;

    memset(oc, 0, sizeof(*oc));
    oc->s = s;
    if (!s->enabled || s->xrun_generation || !sub || !sub->runtime ||
        !sub->runtime->dma_area || !snd_pcm_running(sub))
        return;
    oc->rt = sub->runtime;
    oc->active = true;
    oc->available = zg01_playback_available(READ_ONCE(oc->rt->control->appl_ptr),
                         s->queued_ptr, oc->rt->boundary, oc->rt->buffer_size);
}

static void zg01_stream_xrun(struct zg01_stream *s)
{
    if (s->enabled && !s->xrun_generation) {
        s->xrun_generation = s->generation;
        queue_work(zg01_period_wq, &s->xrun_work);
    }
}

static void zg01_feedback_xrun_locked(struct zg01_dev *dev, bool include_capture)
{
    int first, last, i;

    dev->feedback_fault = true;
    zg01_chain_drain_locked(&dev->out_chain);
    dev->out_chain.stats.driver_xruns++;

    /* Stop only the transport that actually died.  OUT-side faults
     * (submit failure, starvation, plan overflow) must not tear down
     * capture; IN-chain death kills both. */
    if (include_capture) {
        first = 0;
        last = ZG01_N_STREAMS - 1;
    } else {
        first = ZG01_GAME;
        last = ZG01_VOICE_OUT;
    }
    for (i = first; i <= last; i++) {
        if (dev->streams[i].enabled && !dev->streams[i].xrun_generation) {
            dev->streams[i].xrun_generation = dev->streams[i].generation;
            queue_work(zg01_period_wq, &dev->streams[i].xrun_work);
        }
    }
}

/* OUT-chain faults only: playback pacing died, capture stays up. */
static void zg01_feedback_xrun(struct zg01_dev *dev)
{
    zg01_feedback_xrun_locked(dev, false);
}

/* Fault playback and notify capture too. This does not drain IN:
 * callers do that separately for terminal IN errors or resubmit failure.
 * Invalid feedback also uses this notification scope without killing IN. */
static void zg01_feedback_xrun_all(struct zg01_dev *dev)
{
    zg01_feedback_xrun_locked(dev, true);
}

/* Experimental release heuristic: first valid IN plan plus settle.
 * Playback-only starts briefly run IN (in_assist) for this observation.
 * Valid transport does not prove audible readiness, and digital silence
 * does not rule out firmware-generated pops. dev->lock must be held. */
#define ZG01_PRIME_READY_SETTLE_MS 50

static bool zg01_prime_ready(struct zg01_dev *dev, u64 now)
{
    if (!dev->prime_in_first_ns)
        return false;
    return now - dev->prime_in_first_ns >=
        (u64)ZG01_PRIME_READY_SETTLE_MS * NSEC_PER_MSEC;
}

/* dev->lock covers submit as well as DRAINING publication: IN callbacks must
 * never submit OUT after its stop path has begun draining. Maximum two
 * submitted OUT URBs bounds copy-ahead below the smallest playback ring. */
static void zg01_feedback_pump(struct zg01_dev *dev)
{
    struct zg01_chain *c = &dev->out_chain;
    struct zg01_feedback_queue *q = &dev->feedback;
    struct zg01_feedback_plan plan;
    struct out_consumer oc[2];
    unsigned int limit[2];
    unsigned int id, total, used[2], i, f, n, j;
    struct urb *urb;
    bool nonzero_urb;
    bool submit_has_audio;
    bool priming = false;
    int ret;

    if (dev->feedback_fault)
        return;
    /* Playback-only free-run needs no IN plans. Keep its bookkeeping
     * separate from IN liveness: only valid IN sets feedback_started. */
    /* feedback_started latches on the first valid IN plan (see the
     * push path in zg01_iso_in), not here: invalid IN URBs before
     * any valid plan must keep the bounded startup tolerance below
     * instead of faulting at once. */
    while (!atomic_read(&dev->disconnecting) && zg01_chain_active(c) &&
           atomic_read(&c->inflight) < 2) {
        bool gap_fallback = false;
        bool defer = false;
        bool free_run = !zg01_chain_active(&dev->in_chain);

        nonzero_urb = false;

        if (q->plans && q->pending) {
            total = 0;
            for (i = 0; i < ISO_PKTS_OUT; i++)
                total += q->plan[q->plan_head].frames[i];
            dev->feedback_gap_urbs = 0;
        } else if (free_run ||
                   (dev->have_last_plan && dev->feedback_started) ||
                   (!dev->feedback_started &&
                    zg01_chain_active(&dev->in_chain))) {
            /* Submit nominal cadence during startup and short IN gaps.
             * Windows ETW shows full-size initial transfers after EP0
             * setup (../captures/zg01-cycle-transfers.tsv); it does not
             * establish internal feedback policy or audible readiness.
             * The bound below caps an active but unresponsive IN path. */
            total = 0;
            if (dev->have_last_plan && dev->feedback_started) {
                for (i = 0; i < ISO_PKTS_OUT; i++)
                    total += dev->last_plan.frames[i];
            } else {
                /* No plan yet: size from the nominal six-frame cadence. */
                total = 6 * ISO_PKTS_OUT;
            }
            gap_fallback = true;
            /* Bound fallback to ~500 ms without a fresh valid plan,
             * then fault playback instead of repeating stale timing.
             * EXPERIMENT: no IN chain means no plans can ever arrive;
             * the bound does not apply. */
            if (!free_run &&
                dev->feedback_gap_urbs >= ZG01_GAP_FALLBACK_MAX_URBS) {
                dev->out_chain.stats.feedback_starved++;
                zg01_feedback_xrun(dev);
                return;
            }
        } else {
            break;
        }
        snap_consumer(dev, ZG01_GAME, &oc[0]);
        snap_consumer(dev, ZG01_VOICE_OUT, &oc[1]);
        for (n = 0; n < 2; n++) {
            struct zg01_stream *s = oc[n].s;

            /* START precedes ALSA's PREPARED -> RUNNING transition.  A
             * starting consumer contributes silence for now; it must not
             * stall the already-running sibling's submissions (a drain
             * and burst catch-up drives hw_ptr into appl_ptr). */
            if (s->enabled && !oc[n].active) {
                limit[n] = 0;
                continue;
            }
            if (!oc[n].active ||
                oc[n].rt->status->state == SNDRV_PCM_STATE_DRAINING) {
                limit[n] = oc[n].available;   /* drain consumes all */
                continue;
            }
            /* Never build an URB from less than a full URB of committed
             * frames: the tail would be padded with silence, a periodic
             * audible gap.  Retire all remaining committed frames only for
             * the DRAINING case above. */
            limit[n] = oc[n].available;
            if (limit[n] < total) {
                c->stats.playback_waits++;
                /* A short URB with audio still in flight is a transient
                 * refill race, not an underrun: defer so the next userspace
                 * write or completion refills instead of emitting silence.
                 * Only a fully drained stream is a real underrun. */
                if (s->queued_pos != s->pcm_pos)
                    defer = true;
                else
                    zg01_stream_xrun(s);
            }
        }
        if (defer) {
            c->stats.playback_defer++;
            return;
        }
        if (gap_fallback)
            c->stats.feedback_starved++;
        if (gap_fallback) {
            /* Startup has no last plan yet; force nominal sizing here
             * (the loop below rewrites the same values either way). */
            if (dev->have_last_plan && dev->feedback_started)
                plan = dev->last_plan;
            else
                memset(&plan, 6, sizeof(plan));
            if (!zg01_feedback_pending_take(q, &id))
                return;
        } else if (!zg01_feedback_take(q, &plan, &id)) {
            return;
        }
        /* Follow each validated IN plan. Windows captures show occasional
         * seven-frame inserts. This restores measured clock compensation
         * for an A/B test against the fixed six-frame workaround. */
        urb = c->urbs[id];
        memset(urb->transfer_buffer, 0, c->iso_pkts * c->iso_pkt_size);
        used[0] = used[1] = 0;
        /* Liveness assist expiry: once IN has covered the discard
         * window, take it down unless real capture adopted it. Same
         * unconditional-clear contract as the prime-release path. */
        if (dev->assist_deadline_ns &&
            ktime_get_ns() >= dev->assist_deadline_ns) {
            dev->assist_deadline_ns = 0;
            if (dev->in_assist) {
                dev->in_assist = false;
                if (!dev->streams[ZG01_VOICE_IN].running)
                    zg01_chain_drain_locked(&dev->in_chain);
            }
        }
        /* Priming window: the device discards early audio. Consume and
         * drop the application's frames (silence stays in the buffer)
         * so ALSA's clock advances normally through the window.
         * Release on the transport heuristic or at the deadline cap. */
        if (dev->prime_deadline_ns) {
            u64 now = ktime_get_ns();

            if (zg01_prime_ready(dev, now) || now >= dev->prime_deadline_ns) {
                dev->prime_deadline_ns = 0;
                dev->prime_ready_ns = now;
                /* Both consumers drop frames during priming; record
                 * whichever playback stream is supplying audio. */
                dev->prime_released_frame =
                    dev->streams[ZG01_GAME].enabled ?
                        dev->streams[ZG01_GAME].queued_pos :
                        dev->streams[ZG01_VOICE_OUT].queued_pos;
                /* Assist IN's job is done: it existed only to clock
                 * this release. Clear unconditionally: if capture
                 * adopted the IN chain mid-prime, a later capture
                 * STOP must see consumers(IN) == false and take IN
                 * down; a latched in_assist would keep IN streaming
                 * during pure playback and reintroduce the firmware
                 * IN-restart pops. */
                if (dev->in_assist) {
                    dev->in_assist = false;
                    if (!dev->streams[ZG01_VOICE_IN].running)
                        zg01_chain_drain_locked(&dev->in_chain);
                }
            } else {
                priming = true;
            }
        }
        for (i = 0; i < ISO_PKTS_OUT; i++) {
            u8 *pkt = urb->transfer_buffer + i * ISO_PKT_SIZE_OUT;

            urb->iso_frame_desc[i].length = plan.frames[i] * 40;
            urb->iso_frame_desc[i].actual_length = 0;
            urb->iso_frame_desc[i].status = 0;
            for (f = 0; f < plan.frames[i]; f++) {
                for (n = 0; n < 2; n++) {
                    unsigned int off;
                    struct zg01_stream *s = oc[n].s;

                    if (!oc[n].active || used[n] >= limit[n])
                        continue;
                    off = ((s->queued_pos + used[n]) % oc[n].rt->buffer_size) * 8;
                    if (!priming) {
                        memcpy(pkt + f * 40 + (n == 0 ? 8 : 0),
                               oc[n].rt->dma_area + off, 8);
                        for (j = 0; j < 8; j++) {
                            if (oc[n].rt->dma_area[off + j]) {
                                nonzero_urb = true;
                                if (!c->stats.first_nonzero_copy_ns) {
                                    c->stats.first_nonzero_copy_ns =
                                        ktime_get_ns();
                                    c->stats.first_nonzero_copy_frame =
                                        s->queued_pos + used[n];
                                }
                                break;
                            }
                        }
                    }
                    used[n]++;
                }
            }
        }
        for (n = 0; n < 2; n++) {
            if (oc[n].active && used[n] < total)
                c->stats.silence_frames += total - used[n];
            c->completed_frames[id][n] = used[n];
            c->generation[id][n] = oc[n].s->generation;
        }
        atomic_inc(&c->inflight);
        submit_has_audio = nonzero_urb;
        ret = usb_submit_urb(urb, GFP_ATOMIC);
        if (ret) {
            atomic_dec(&c->inflight);
            c->stats.feedback_submit_errors++;
            zg01_feedback_pending(q, id);
            zg01_feedback_xrun(dev);
            return;
        }
        if (submit_has_audio && !c->stats.first_nonzero_submit_ns)
            c->stats.first_nonzero_submit_ns = ktime_get_ns();
        if (gap_fallback && !free_run)
            dev->feedback_gap_urbs++;
        for (n = 0; n < 2; n++) {
            if (!used[n])
                continue;
            oc[n].s->queued_pos += used[n];
            oc[n].s->queued_ptr = (oc[n].s->queued_ptr + used[n]) % oc[n].rt->boundary;
        }
    }
}

static void zg01_iso_out(struct urb *urb)
{
    struct zg01_chain *c = urb->context;
    struct zg01_dev *dev = c->dev;
    unsigned long flags;
    unsigned int id, n;
    u64 now;
    bool terminal = urb->status == -ENOENT || urb->status == -ECONNRESET ||
                    urb->status == -ESHUTDOWN;

    spin_lock_irqsave(&dev->lock, flags);
    now = ktime_get_ns();
    zg01_usb_stats_account(&c->stats, urb, false, now);
    if (!urb->status && !c->stats.first_out_completion_ns)
        c->stats.first_out_completion_ns = now;
    for (id = 0; id < MAX_URBS && c->urbs[id] != urb; id++)
        ;
    atomic_dec(&c->inflight);
    if (terminal && zg01_chain_active(c) && !atomic_read(&dev->disconnecting)) {
        zg01_chain_drain_locked(c);
        if (c == &dev->in_chain)
            zg01_feedback_xrun_all(dev);
        else
            zg01_feedback_xrun(dev);
    }
    if (id == MAX_URBS || terminal || !zg01_chain_active(c) ||
        atomic_read(&dev->disconnecting))
        goto unlock;
    for (n = 0; n < 2; n++) {
        struct zg01_stream *s = &dev->streams[n];
        unsigned int frames = c->completed_frames[id][n];

        /* Retire submitted samples once, including lost USB packets;
         * never replay a partly successful URB. USB errors remain counted. */
        if (frames && s->enabled && s->generation == c->generation[id][n]) {
            s->pcm_pos += frames;
            /* Also wakes a drain shorter than period_size. */
            queue_work(zg01_period_wq, &s->period_work);
        }
        c->completed_frames[id][n] = 0;
    }
    zg01_feedback_pending(&dev->feedback, id);
    if (!dev->feedback.plans)
        c->stats.feedback_starved++;
    zg01_feedback_pump(dev);
unlock:
    spin_unlock_irqrestore(&dev->lock, flags);
}

/* IN is a shared clock source, independent of whether capture is open.
 * Header word 1 is payload bytes; word 0 is an observed packet sequence.
 * Only validated 5/6/7-frame packets supply capture or feedback. */
static void zg01_iso_in(struct urb *urb)
{
    struct zg01_chain *c = urb->context;
    struct zg01_dev *dev = c->dev;
    struct zg01_stream *s = &dev->streams[ZG01_VOICE_IN];
    struct snd_pcm_substream *sub;
    struct snd_pcm_runtime *rt;
    struct zg01_feedback_plan plan = {0};
    unsigned long flags;
    unsigned int old_pos, i, f;
    bool valid = urb->status == 0 && urb->number_of_packets == ISO_PKTS_IN;

    spin_lock_irqsave(&dev->lock, flags);
    zg01_usb_stats_account(&c->stats, urb, true, ktime_get_ns());
    zg01_in_trace_account(&dev->in_trace, urb, ktime_get_ns());
    sub = s->substream;
    rt = (s->enabled && sub && sub->runtime && sub->runtime->dma_area &&
          snd_pcm_running(sub)) ? sub->runtime : NULL;
    old_pos = s->pcm_pos;
    if (zg01_chain_active(c) && !atomic_read(&dev->disconnecting)) {
        for (i = 0; i < urb->number_of_packets && i < ISO_PKTS_IN; i++) {
            const struct usb_iso_packet_descriptor *p = &urb->iso_frame_desc[i];
            unsigned int frames = 0;

            if (!urb->status && urb->transfer_buffer_length >= 0)
                frames = zg01_packet_frames(urb->transfer_buffer,
                            urb->transfer_buffer_length, p->offset,
                            p->actual_length, p->length, p->status);
            if (!frames) {
                c->stats.feedback_invalid++;
                valid = false;
                continue;
            }
            c->stats.feedback_valid++;
            plan.frames[i] = frames;
            if (!rt)
                continue;
            for (f = 0; f < frames; f++) {
                unsigned int off = (s->pcm_pos % rt->buffer_size) * 8;

                memcpy(rt->dma_area + off,
                       urb->transfer_buffer + p->offset + 8 + f * 16, 8);
                s->pcm_pos++;
            }
        }
        if (rt && s->pcm_pos != old_pos)
            queue_work(zg01_period_wq, &s->period_work);
        if (zg01_chain_active(&dev->out_chain) &&
            (dev->feedback.pending || atomic_read(&dev->out_chain.inflight)) &&
            !dev->feedback_fault) {
            if (!valid && (dev->feedback_started ||
                          ++dev->feedback_startup_urbs >= MAX_URBS))
                zg01_feedback_xrun_all(dev);
        }
        if (valid && !dev->feedback_fault && zg01_chain_active(&dev->out_chain) &&
            (dev->feedback.pending || atomic_read(&dev->out_chain.inflight))) {
            dev->last_plan = plan;
            dev->have_last_plan = true;
            /* First valid plan ends startup: later invalid IN URBs
             * fault at once, and the gap-fallback branch below can
             * run on this plan. */
            dev->feedback_started = true;
            if (dev->prime_deadline_ns && !dev->prime_in_first_ns)
                dev->prime_in_first_ns = ktime_get_ns();
            if (!zg01_feedback_push(&dev->feedback, &plan)) {
                c->stats.feedback_overflow++;
                zg01_feedback_xrun(dev);
            }
        }
        zg01_feedback_pump(dev);
    }
    chain_resubmit(c, urb);
    spin_unlock_irqrestore(&dev->lock, flags);
}

/* ================================================================== */
/* PCM operations                                                      */
/* ================================================================== */

static int zg01_pcm_open(struct snd_pcm_substream *substream)
{
    struct zg01_stream *s = sub_to_stream(substream);
    struct zg01_dev *dev = s->dev;
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned long now = jiffies;
    int ret = 0;

    mutex_lock(&dev->state_mutex);

    if (atomic_read(&dev->disconnecting)) {
        ret = -ENODEV;
        goto unlock;
    }

    /* Log rate limiting for audio-system probing */
    if (time_before(now, dev->last_open_jiffies + msecs_to_jiffies(1000)))
        dev->open_count++;
    else
        dev->open_count = 1;
    dev->last_open_jiffies = now;

    /* Capture open takes ownership of the IN chain: drop any live
     * keepalive hold so a later capture STOP does not find the flag
     * latched and leave IN streaming forever. */
    if (s->direction == SNDRV_PCM_STREAM_CAPTURE) {
        unsigned long flags;

        spin_lock_irqsave(&dev->lock, flags);
        WRITE_ONCE(dev->in_hold, false);
        spin_unlock_irqrestore(&dev->lock, flags);
    }

    runtime->hw.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
                       SNDRV_PCM_INFO_INTERLEAVED |
                       SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_BATCH;
    runtime->hw.formats = SNDRV_PCM_FMTBIT_S32_LE;
    runtime->hw.channels_min = 2;
    runtime->hw.channels_max = 2;
    runtime->hw.periods_min = 2;
    runtime->hw.periods_max = 64;

    if (s->direction == SNDRV_PCM_STREAM_CAPTURE) {
        runtime->hw.rates = SNDRV_PCM_RATE_48000;
        runtime->hw.rate_min = 48000;
        runtime->hw.rate_max = 48000;
        runtime->hw.buffer_bytes_max = PCM_BUFFER_BYTES_MAX_VOICE;
        runtime->hw.period_bytes_min = PCM_PERIOD_BYTES_MIN_VOICE;
        runtime->hw.period_bytes_max = PCM_PERIOD_BYTES_MAX_VOICE;
        ret = snd_pcm_hw_constraint_step(runtime, 0,
                                         SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 48);
        if (ret)
            goto unlock;
        ret = snd_pcm_hw_constraint_step(runtime, 0,
                                         SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 48);
    } else {
        runtime->hw.rates = SNDRV_PCM_RATE_48000;
        runtime->hw.rate_min = 48000;
        runtime->hw.rate_max = 48000;
        runtime->hw.buffer_bytes_max = PCM_BUFFER_BYTES_MAX_GAME;
        runtime->hw.period_bytes_min = PCM_PERIOD_BYTES_MIN_GAME;
        runtime->hw.period_bytes_max = PCM_PERIOD_BYTES_MAX_GAME;
        ret = snd_pcm_hw_constraint_step(runtime, 0,
                                         SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 1536);
        if (ret)
            goto unlock;
        ret = snd_pcm_hw_constraint_step(runtime, 0,
                                         SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 96);
    }
    if (ret)
        goto unlock;

    s->opened = true;
    dev_info(&dev->udev->dev, "open %s\n", stream_name(s));

unlock:
    mutex_unlock(&dev->state_mutex);
    return ret;
}

/* Detach this consumer under dev->lock before flushing deferred work.
 * Sibling transport may continue, but callbacks cannot acquire this
 * substream after detachment. Flush without state_mutex: cleanup needs it. */
static int zg01_pcm_close(struct snd_pcm_substream *substream)
{
    struct zg01_stream *s = sub_to_stream(substream);
    struct zg01_dev *dev = s->dev;
    struct zg01_chain *c = (s->direction == SNDRV_PCM_STREAM_CAPTURE)
        ? &dev->in_chain : &dev->out_chain;
    bool arm_keepalive_in = false;
    unsigned long flags;

    /* Close can extend the hold again after hw_free. Join any expired
     * callback outside state_mutex before publishing that extension. */
    cancel_delayed_work_sync(&dev->out_chain.quiesce_work);
    cancel_delayed_work_sync(&dev->in_chain.quiesce_work);
    mutex_lock(&dev->state_mutex);
    s->opened = false;
    s->running = false;
    spin_lock_irqsave(&dev->lock, flags);
    s->enabled = false;
    s->generation++;
    s->xrun_generation = 0;
    s->substream = NULL;
    if (c == &dev->in_chain)
        dev->out_silence = false;
    spin_unlock_irqrestore(&dev->lock, flags);
    if (c == &dev->out_chain && zg01_hold_across_close(dev)) {
        /* Keepalive hold: see hw_free. Arming runs after the mutex. */
        mod_delayed_work(zg01_cleanup_wq, &c->quiesce_work,
                         msecs_to_jiffies(keepalive_ms));
        arm_keepalive_in = true;
    } else if (!chain_consumers_running(dev, c)) {
        zg01_chain_stop(dev, c);
    }
    if (!chain_consumers_running(dev, &dev->in_chain))
        zg01_chain_stop(dev, &dev->in_chain);
    if (c == &dev->in_chain &&
        !chain_consumers_running(dev, &dev->out_chain))
        zg01_chain_stop(dev, &dev->out_chain);
    mutex_unlock(&dev->state_mutex);

    if (arm_keepalive_in)
        zg01_keepalive_arm(dev);

    flush_work(&dev->out_chain.cleanup_work);
    flush_work(&dev->in_chain.cleanup_work);
    flush_work(&s->period_work);
    flush_work(&s->xrun_work);

    spin_lock_irqsave(&dev->lock, flags);
    s->substream = NULL;
    spin_unlock_irqrestore(&dev->lock, flags);

    return 0;
}

static int zg01_pcm_hw_params(struct snd_pcm_substream *substream,
                              struct snd_pcm_hw_params *hw_params)
{
    if (params_channels(hw_params) != 2)
        return -EINVAL;
    if (params_format(hw_params) != SNDRV_PCM_FORMAT_S32_LE)
        return -EINVAL;
    if (params_rate(hw_params) != 48000)
        return -EINVAL;

    return 0;
}

static int zg01_pcm_hw_free(struct snd_pcm_substream *substream)
{
    struct zg01_stream *s = sub_to_stream(substream);
    struct zg01_dev *dev = s->dev;
    struct zg01_chain *c = (s->direction == SNDRV_PCM_STREAM_CAPTURE)
        ? &dev->in_chain : &dev->out_chain;
    bool arm_keepalive_in = false;
    unsigned long flags;

    /* Join old quiesce callbacks before extending the hold. Re-arming
     * delayed work does not revoke an already-running invocation.
     * Join outside state_mutex because quiesce takes that mutex. */
    cancel_delayed_work_sync(&dev->out_chain.quiesce_work);
    cancel_delayed_work_sync(&dev->in_chain.quiesce_work);

    mutex_lock(&dev->state_mutex);
    s->running = false;
    spin_lock_irqsave(&dev->lock, flags);
    s->enabled = false;
    s->generation++;
    s->xrun_generation = 0;
    s->substream = NULL;
    if (c == &dev->out_chain &&
        !dev->streams[ZG01_GAME].running &&
        !dev->streams[ZG01_VOICE_OUT].running)
        dev->in_assist = false;
    if (c == &dev->in_chain)
        dev->out_silence = false;
    spin_unlock_irqrestore(&dev->lock, flags);
    if (c == &dev->out_chain && zg01_hold_across_close(dev)) {
        /* Keepalive: leave OUT cycling silence with no substream;
         * the quiesce timer stops it keepalive_ms later. IN joins
         * the hold: the device output path needs IN isoc liveness.
         * Arming runs AFTER state_mutex is released — chain_start
         * takes that mutex. */
        mod_delayed_work(zg01_cleanup_wq, &c->quiesce_work,
                         msecs_to_jiffies(keepalive_ms));
        arm_keepalive_in = true;
    } else if (!chain_consumers_running(dev, c)) {
        zg01_chain_stop(dev, c);
    }
    if (!chain_consumers_running(dev, &dev->in_chain))
        zg01_chain_stop(dev, &dev->in_chain);
    if (c == &dev->in_chain &&
        !chain_consumers_running(dev, &dev->out_chain))
        zg01_chain_stop(dev, &dev->out_chain);
    mutex_unlock(&dev->state_mutex);

    if (arm_keepalive_in)
        zg01_keepalive_arm(dev);

    flush_work(&dev->out_chain.cleanup_work);
    flush_work(&dev->in_chain.cleanup_work);
    flush_work(&s->period_work);
    flush_work(&s->xrun_work);

    /* dma_area is freed after this callback returns; make sure no
     * callback can still read it. */
    spin_lock_irqsave(&dev->lock, flags);
    s->substream = NULL;
    spin_unlock_irqrestore(&dev->lock, flags);

    /* Keep the chains allocated: next prepare reuses them. */
    return 0;
}

static int zg01_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct zg01_stream *s = sub_to_stream(substream);
    struct zg01_dev *dev = s->dev;
    unsigned long flags;
    int ret;

    mutex_lock(&dev->state_mutex);

    if (atomic_read(&dev->disconnecting)) {
        mutex_unlock(&dev->state_mutex);
        return -ENODEV;
    }

    ret = zg01_chain_wait_cleanup(dev, &dev->in_chain);
    if (!ret)
        ret = zg01_chain_wait_cleanup(dev, &dev->out_chain);
    if (ret) {
        mutex_unlock(&dev->state_mutex);
        return ret;
    }

    /*
     * Device initialization (vendor handshake + rate).  The Magic
     * Sequence resets both interfaces to alt 0, killing every live URB
     * — run it only when NO chain is streaming.  state_mutex makes this
     * check atomic against triggers on the sibling PCMs.
     *
     * The clock is DEVICE-WIDE (one UAC2 clock source shared by
     * both interfaces).  Always initialize at 48 kHz — a Voice In
     * first-open at 16 kHz must not mis-clock the 48 kHz-only Game
     * playback (old driver effectively behaved the same: first init
     * always requested 48000).
     */
    if (!dev->device_initialized &&
        READ_ONCE(dev->out_chain.state) == ZG01_CHAIN_STOPPED &&
        READ_ONCE(dev->in_chain.state) == ZG01_CHAIN_STOPPED) {
        ret = zg01_set_rate(dev, 48000);
        if (ret < 0)
            dev_warn(&dev->udev->dev, "set_rate failed: %d\n", ret);
        dev->device_initialized = true;
    }

    /* Prepare both endpoints: capture needs paired OUT, and playback
     * may request IN assist. Never reset a live interface when a sibling
     * PCM joins. Reuse an interface that already has the required alt. */
    if (READ_ONCE(dev->in_chain.state) == ZG01_CHAIN_STOPPED) {
        struct usb_interface *iface;

        iface = usb_ifnum_to_if(dev->udev, 2);
        if (!iface) {
            mutex_unlock(&dev->state_mutex);
            return -ENODEV;
        }
        if (iface->cur_altsetting->desc.bAlternateSetting != 1) {
            ret = usb_set_interface(dev->udev, 2, 1);
            if (ret < 0) {
                mutex_unlock(&dev->state_mutex);
                return ret;
            }
        }
    }
    ret = zg01_chain_alloc(dev, &dev->in_chain, ZG01_EP_IN, 2,
                           ISO_PKTS_IN, ISO_PKT_SIZE_IN, false);
    if (!ret) {
        if (READ_ONCE(dev->out_chain.state) == ZG01_CHAIN_STOPPED) {
            struct usb_interface *iface;

            iface = usb_ifnum_to_if(dev->udev, 1);
            if (!iface) {
                mutex_unlock(&dev->state_mutex);
                return -ENODEV;
            }
            if (iface->cur_altsetting->desc.bAlternateSetting != 1) {
                ret = usb_set_interface(dev->udev, 1, 1);
                if (ret < 0) {
                    mutex_unlock(&dev->state_mutex);
                    return ret;
                }
            }
        }
        ret = zg01_chain_alloc(dev, &dev->out_chain, ZG01_EP_OUT, 1,
                               ISO_PKTS_OUT, ISO_PKT_SIZE_OUT, true);
    }
    if (ret) {
        mutex_unlock(&dev->state_mutex);
        return ret;
    }

    /*
     * Publish the substream and reset the position under dev->lock.
     * The callback advances this position for every frame submitted to
     * the endpoint. ALSA compares it with appl_ptr to detect underruns.
     */
    spin_lock_irqsave(&dev->lock, flags);
    s->substream = substream;
    s->pcm_pos = 0;
    s->queued_pos = 0;
    s->generation++;
    s->enabled = false;
    s->xrun_generation = 0;
    spin_unlock_irqrestore(&dev->lock, flags);

    mutex_unlock(&dev->state_mutex);
    return 0;
}

static int zg01_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct zg01_stream *s = sub_to_stream(substream);
    struct zg01_dev *dev = s->dev;
    struct zg01_chain *c = (s->direction == SNDRV_PCM_STREAM_CAPTURE)
        ? &dev->in_chain : &dev->out_chain;
    bool draining;
    int ret;
    unsigned long flags;

    mutex_lock(&dev->state_mutex);

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        if (!c->allocated) {
            mutex_unlock(&dev->state_mutex);
            return -ENOMEM;
        }
        s->running = true;
        spin_lock_irqsave(&dev->lock, flags);
        s->enabled = true;
        s->generation++;
        s->xrun_generation = 0;
        s->queued_pos = s->pcm_pos;
        s->queued_ptr = substream->runtime->status->hw_ptr;
        /* Capture owns OUT even when it joins existing playback. */
        if (c == &dev->in_chain)
            dev->out_silence = true;
        spin_unlock_irqrestore(&dev->lock, flags);
        mutex_unlock(&dev->state_mutex);

        /* May sleep (cleanup drain); state re-checked inside. */
        ret = zg01_chain_start(dev, c);
        /* Start or adopt OUT for capture. Windows capture-only traffic
         * uses both iso pipes (../captures/AUDIO_PACKET_ANALYSIS.md).
         * Our OUT pump supplies silence when no playback runs. */
        if (!ret && c == &dev->in_chain)
            ret = zg01_chain_start(dev, &dev->out_chain);
        /* EXPERIMENT: IN runs for capture, and briefly as prime
         * readiness signal (in_assist) on fresh playback-only
         * epochs. Warm adoption of a RUNNING OUT chain starts no new
         * epoch and needs no assist. */
        if (!ret && c == &dev->out_chain) {
            ret = zg01_chain_start(dev, &dev->in_chain);
            if (ret == -ECANCELED)
                ret = 0;
        }
        if (!ret) {
            /* Submit OUT immediately in every mode, including capture.
             * IN callbacks are not required to kick the initial batch. */
            spin_lock_irqsave(&dev->lock, flags);
            zg01_feedback_pump(dev);
            spin_unlock_irqrestore(&dev->lock, flags);
        }
        if (ret) {
            mutex_lock(&dev->state_mutex);
            s->running = false;
            spin_lock_irqsave(&dev->lock, flags);
            s->enabled = false;
            s->xrun_generation = 0;
            if (c == &dev->in_chain)
                dev->out_silence = false;
            if (c == &dev->out_chain &&
                !dev->streams[ZG01_GAME].running &&
                !dev->streams[ZG01_VOICE_OUT].running)
                dev->in_assist = false;
            spin_unlock_irqrestore(&dev->lock, flags);
            if (!chain_consumers_running(dev, &dev->out_chain))
                zg01_chain_stop(dev, &dev->out_chain);
            if (!chain_consumers_running(dev, &dev->in_chain))
                zg01_chain_stop(dev, &dev->in_chain);
            mutex_unlock(&dev->state_mutex);
            dev_err(&dev->udev->dev, "chain start failed: %d\n", ret);
        }
        return ret;

    case SNDRV_PCM_TRIGGER_STOP:
        /* Clip-gap warm hold: every playback STOP keeps the OUT chain
         * cycling silence. Burst counters stay for diagnostics only. */
        if (time_before(jiffies,
                        s->last_trigger_jiffies + msecs_to_jiffies(100)))
            s->trigger_count++;
        else
            s->trigger_count = 1;
        s->last_trigger_jiffies = jiffies;
        draining = substream->runtime &&
            substream->runtime->status->state == SNDRV_PCM_STATE_DRAINING;

        s->running = false;
        spin_lock_irqsave(&dev->lock, flags);
        s->enabled = false;
        s->generation++;
        s->xrun_generation = 0;
        /* Prime assist ends with the playback that requested it. */
        if (c == &dev->out_chain &&
            !dev->streams[ZG01_GAME].running &&
            !dev->streams[ZG01_VOICE_OUT].running)
            dev->in_assist = false;
        /* Capture-owned OUT silence ends with capture. */
        if (c == &dev->in_chain)
            dev->out_silence = false;
        spin_unlock_irqrestore(&dev->lock, flags);
        if (c == &dev->out_chain && zg01_hold_warm(draining) &&
            !chain_consumers_running(dev, c)) {
            /* Keep the chain cycling (silence); the quiesce timer
             * stops it if no START follows. */
            mod_delayed_work(zg01_cleanup_wq, &c->quiesce_work,
                             msecs_to_jiffies(quiesce_ms));
        } else if (!chain_consumers_running(dev, c)) {
            zg01_chain_stop(dev, c);
        }
        if (!chain_consumers_running(dev, &dev->in_chain))
            zg01_chain_stop(dev, &dev->in_chain);
        if (c == &dev->in_chain &&
            !chain_consumers_running(dev, &dev->out_chain))
            zg01_chain_stop(dev, &dev->out_chain);
        mutex_unlock(&dev->state_mutex);
        return 0;

    case SNDRV_PCM_TRIGGER_SUSPEND:
        s->running = false;
        spin_lock_irqsave(&dev->lock, flags);
        s->enabled = false;
        s->generation++;
        s->xrun_generation = 0;
        if (c == &dev->out_chain)
            dev->in_assist = false;
        if (c == &dev->in_chain)
            dev->out_silence = false;
        spin_unlock_irqrestore(&dev->lock, flags);
        /* Async cancel only: _sync deadlocks under state_mutex. */
        cancel_delayed_work(&c->quiesce_work);
        cancel_delayed_work(&dev->in_chain.quiesce_work);
        if (!chain_consumers_running(dev, c))
            zg01_chain_stop(dev, c);
        if (!chain_consumers_running(dev, &dev->in_chain))
            zg01_chain_stop(dev, &dev->in_chain);
        if (c == &dev->in_chain &&
            !chain_consumers_running(dev, &dev->out_chain))
            zg01_chain_stop(dev, &dev->out_chain);
        mutex_unlock(&dev->state_mutex);
        return 0;

    default:
        mutex_unlock(&dev->state_mutex);
        return -EINVAL;
    }
}

static snd_pcm_uframes_t zg01_pcm_pointer(struct snd_pcm_substream *substream)
{
    struct zg01_stream *s = sub_to_stream(substream);
    struct zg01_dev *dev = s->dev;
    unsigned long pos;
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    pos = s->pcm_pos;
    spin_unlock_irqrestore(&dev->lock, flags);

    return pos % substream->runtime->buffer_size;
}

static int zg01_pcm_ack(struct snd_pcm_substream *substream)
{
    struct zg01_dev *dev = sub_to_stream(substream)->dev;
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    zg01_feedback_pump(dev);
    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

static int zg01_pcm_ioctl(struct snd_pcm_substream *substream,
                          unsigned int cmd, void *arg)
{
    return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static const struct snd_pcm_ops zg01_pcm_ops = {
    .open = zg01_pcm_open,
    .close = zg01_pcm_close,
    .ioctl = zg01_pcm_ioctl,
    .hw_params = zg01_pcm_hw_params,
    .hw_free = zg01_pcm_hw_free,
    .prepare = zg01_pcm_prepare,
    .trigger = zg01_pcm_trigger,
    .pointer = zg01_pcm_pointer,
    .ack = zg01_pcm_ack,
};

/* ================================================================== */
/* Card / PCM creation                                                 */
/* ================================================================== */

static int zg01_new_pcm(struct zg01_dev *dev, enum zg01_stream_id id,
                        const char *name, int playback, int capture,
                        int buffer_bytes_max)
{
    struct zg01_stream *s = &dev->streams[id];
    struct snd_pcm *pcm;
    int ret;

    ret = snd_pcm_new(dev->card, name, s->pcm_device, playback, capture, &pcm);
    if (ret)
        return ret;

    snd_pcm_set_ops(pcm, playback ? SNDRV_PCM_STREAM_PLAYBACK
                                  : SNDRV_PCM_STREAM_CAPTURE, &zg01_pcm_ops);
    pcm->private_data = s;
    pcm->nonatomic = 1;
    strscpy(pcm->name, name, sizeof(pcm->name));

    dev->pcm_instances[id] = pcm;
    return 0;
}

int zg01_create_pcm_devices(struct zg01_dev *dev)
{
    int ret;

    /* The pending-id ring requeues one id per recycled URB; a depth
     * smaller than the URB count would silently drop ids. */
    BUILD_BUG_ON(ZG01_FB_DEPTH < MAX_URBS);

    ret = zg01_new_pcm(dev, ZG01_GAME, "ZG01 Game Out", 1, 0,
                       PCM_BUFFER_BYTES_MAX_GAME);
    if (ret)
        return ret;

    ret = zg01_new_pcm(dev, ZG01_VOICE_OUT, "ZG01 Voice Out", 1, 0,
                       PCM_BUFFER_BYTES_MAX_GAME);
    if (ret)
        return ret;

    ret = zg01_new_pcm(dev, ZG01_VOICE_IN, "ZG01 Voice In", 0, 1,
                       PCM_BUFFER_BYTES_MAX_VOICE);
    if (ret)
        return ret;

    for (int i = 0; i < ZG01_N_STREAMS; i++) {
        unsigned int max = dev->streams[i].direction ==
                           SNDRV_PCM_STREAM_CAPTURE
                               ? PCM_BUFFER_BYTES_MAX_VOICE
                               : PCM_BUFFER_BYTES_MAX_GAME;

        /* Managed buffer at FULL size: the prealloc floor is also the
         * hw_params ceiling (no realloc path), and an 8 KB prealloc
         * clamps userspace to a 1020-frame ring.  On that ring ALSA's
         * stale-pointer threshold (~10.6 ms) sits below routine workqueue
         * latency, which fabricates hw_ptr wraps = false XRUNs. */
        snd_pcm_set_managed_buffer_all(dev->pcm_instances[i],
                                       SNDRV_DMA_TYPE_CONTINUOUS, NULL,
                                       max, max);
    }

    ret = snd_card_ro_proc_new(dev->card, "usb_stats", dev,
                               zg01_usb_stats_read);
    if (ret)
        return ret;
    return snd_card_ro_proc_new(dev->card, "in_trace", dev,
                                zg01_in_trace_read);
}

/* ================================================================== */
/* PM helpers (called from zg01_usb.c)                                 */
/* ================================================================== */

void zg01_suspend_pcm(struct zg01_dev *dev)
{
    int i;

    for (i = 0; i < ZG01_N_STREAMS; i++) {
        if (dev->pcm_instances[i])
            snd_pcm_suspend_all(dev->pcm_instances[i]);
    }
}

void zg01_pm_reset_streams(struct zg01_dev *dev)
{
    int i;

    /*
     * The firmware reset itself across suspend: force the first-prepare
     * path (vendor handshake + rate) on the next open of each stream.
     * Suspend has drained both chains to STOPPED before this reset.
     */
    mutex_lock(&dev->state_mutex);
    for (i = 0; i < ZG01_N_STREAMS; i++)
        dev->streams[i].running = false;
    dev->device_initialized = false;
    mutex_unlock(&dev->state_mutex);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Yamaha ZG01 USB Audio Driver - PCM layer");
