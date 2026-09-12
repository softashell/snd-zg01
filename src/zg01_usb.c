/*
 * Yamaha ZG01 USB Audio Driver - USB glue
 *
 * One USB device, one ALSA card, three PCM devices (Game playback,
 * Voice Out playback, Voice In capture).  Probe anchors on interface 1
 * (shared EP 0x01 playback).  Interface 2 (EP 0x81 capture) is claimed
 * by the same driver instance and creates no extra card.  No global
 * device pointers, no devices_mutex.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>

#include "zg01.h"

/*
 * WQ_PERCPU names the explicit CPU binding of a workqueue. It first appeared
 * in v6.17 and alloc_workqueue() reads it from 6.18 on; the warning that
 * motivated an explicit binding arrived later still (v7.2). Kernels without
 * the identifier bind an unbound-free queue to the local CPU implicitly, which
 * is the behaviour these queues always had. The identifier is an enum
 * constant, not a preprocessor macro, so only a version test can detect it:
 * 0 keeps the legacy implicit per-CPU binding.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define ZG01_WQ_PERCPU WQ_PERCPU
#else
#define ZG01_WQ_PERCPU 0
#endif

struct workqueue_struct *zg01_cleanup_wq;
struct workqueue_struct *zg01_period_wq;

static void zg01_card_private_free(struct snd_card *card)
{
    struct zg01_dev *dev = card->private_data;

    if (dev->udev) {
        usb_put_dev(dev->udev);
        dev->udev = NULL;
    }
}

static int zg01_probe(struct usb_interface *interface,
                      const struct usb_device_id *id);
static void zg01_disconnect(struct usb_interface *interface);
static int zg01_suspend(struct usb_interface *intf, pm_message_t message);
static int zg01_resume(struct usb_interface *intf);
static int zg01_reset_resume(struct usb_interface *intf);

static const struct usb_device_id zg01_table[] = {
    { USB_DEVICE(VENDOR_ID_YAMAHA, PRODUCT_ID_ZG01) },
    { }
};
MODULE_DEVICE_TABLE(usb, zg01_table);

static struct usb_driver zg01_driver = {
    .name          = "zg01_usb",
    .id_table      = zg01_table,
    .probe         = zg01_probe,
    .disconnect    = zg01_disconnect,
    .suspend       = zg01_suspend,
    .resume        = zg01_resume,
    .reset_resume  = zg01_reset_resume,
};

static int zg01_probe(struct usb_interface *interface,
                      const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(interface);
    struct snd_card *card;
    struct zg01_dev *dev;
    int err, i;

    if (interface->cur_altsetting->desc.bInterfaceNumber != 1)
        return 0;

    err = snd_card_new(&interface->dev, -1, "zg01", THIS_MODULE,
                       sizeof(struct zg01_dev), &card);
    if (err)
        return err;

    dev = card->private_data;
    dev->card = card;
    dev->udev = usb_get_dev(udev);
    dev->interface = interface;
    card->private_free = zg01_card_private_free;

    spin_lock_init(&dev->lock);
    mutex_init(&dev->state_mutex);
    atomic_set(&dev->disconnecting, 0);
    atomic_set(&dev->disconnected, 0);
    dev->device_initialized = false;
    dev->last_open_jiffies = 0;
    dev->open_count = 0;

    for (int i = 0; i < ZG01_N_STREAMS; i++) {
        struct zg01_stream *s = &dev->streams[i];

        s->dev = dev;
        s->substream = NULL;
        s->pcm_pos = 0;
        s->opened = false;
        s->running = false;
        s->last_trigger_jiffies = 0;
        s->trigger_count = 0;
        INIT_WORK(&s->period_work, zg01_period_work_fn);
        INIT_WORK(&s->xrun_work, zg01_xrun_work_fn);
    }
    dev->streams[ZG01_GAME].pcm_device = ZG01_PCM_GAME;
    dev->streams[ZG01_GAME].direction = SNDRV_PCM_STREAM_PLAYBACK;
    dev->streams[ZG01_VOICE_OUT].pcm_device = ZG01_PCM_VOICE_OUT;
    dev->streams[ZG01_VOICE_OUT].direction = SNDRV_PCM_STREAM_PLAYBACK;
    dev->streams[ZG01_VOICE_IN].pcm_device = ZG01_PCM_VOICE_IN;
    dev->streams[ZG01_VOICE_IN].direction = SNDRV_PCM_STREAM_CAPTURE;

    dev->out_chain.dev = dev;
    dev->in_chain.dev = dev;
    for (int i = 0; i < MAX_URBS; i++) {
        dev->out_chain.urbs[i] = NULL;
        dev->out_chain.bufs[i] = NULL;
        dev->in_chain.urbs[i] = NULL;
        dev->in_chain.bufs[i] = NULL;
    }
    dev->out_chain.allocated = false;
    dev->in_chain.allocated = false;
    dev->out_chain.state = ZG01_CHAIN_STOPPED;
    dev->in_chain.state = ZG01_CHAIN_STOPPED;
    atomic_set(&dev->out_chain.inflight, 0);
    atomic_set(&dev->in_chain.inflight, 0);
    INIT_WORK(&dev->out_chain.cleanup_work, zg01_chain_cleanup_fn);
    INIT_WORK(&dev->in_chain.cleanup_work, zg01_chain_cleanup_fn);
    INIT_DELAYED_WORK(&dev->keepalive_rearm_work, zg01_keepalive_rearm_fn);
    INIT_DELAYED_WORK(&dev->out_chain.quiesce_work, zg01_chain_quiesce_fn);
    INIT_DELAYED_WORK(&dev->in_chain.quiesce_work, zg01_chain_quiesce_fn);

    strscpy(card->driver, "zg01_usb", sizeof(card->driver));
    strscpy(card->shortname, "ZG01", sizeof(card->shortname));
    strscpy(card->longname, "Yamaha ZG01", sizeof(card->longname));
    strscpy(card->mixername, "ZG01", sizeof(card->mixername));

    err = zg01_init_control(dev);
    if (err) {
        dev_err(&interface->dev, "control init failed: %d\n", err);
        goto err_free_card;
    }

    /* Discovery is best-effort, debug only */
    if (zg01_discover_usb_config(dev))
        dev_warn(&interface->dev, "discovery failed, continuing\n");

    /* Streaming interfaces to alt 0 before PCM registration */
    usb_set_interface(dev->udev, 1, 0);
    usb_set_interface(dev->udev, 2, 0);

    err = zg01_create_pcm_devices(dev);
    if (err) {
        dev_err(&interface->dev, "PCM creation failed: %d\n", err);
        goto err_free_card;
    }

    err = snd_card_register(card);
    if (err) {
        dev_err(&interface->dev, "card registration failed: %d\n", err);
        goto err_free_card;
    }

    usb_set_intfdata(interface, dev);

    /* Claim the vendor bulk interfaces (3: EP 0x02/0x82, 4: EP 0x03/0x83)
     * so snd-usb-audio's useless MIDI devices never take them and the
     * endpoints stay available (mic-monitor protocol lives here). Claim
     * fails with -EBUSY when the generic driver already bound at boot;
     * unbinding 3/4 from snd-usb-audio before loading this module makes
     * the claim stick. intfdata stays NULL: PCM-less claim. */
    for (i = 3; i <= 4; i++) {
        struct usb_interface *aux = usb_ifnum_to_if(udev, i);

        if (!aux)
            continue;
        err = usb_driver_claim_interface(&zg01_driver, aux, NULL);
        if (err && err != -EBUSY)
            dev_info(&interface->dev,
                     "interface %d not claimed: %d\n", i, err);
        else if (!err)
            dev_info(&interface->dev, "claimed interface %d\n", i);
    }

    dev_info(&interface->dev, "ZG01 card created (3 PCM devices)\n");
    return 0;

err_free_card:
    snd_card_free(card);
    return err;
}

static void zg01_disconnect(struct usb_interface *interface)
{
    struct zg01_dev *dev = usb_get_intfdata(interface);
    int i;
    int ret;

    if (!dev)
        return;

    usb_set_intfdata(interface, NULL);

    /* Release claimed vendor interfaces (3/4) when the primary
     * interface disconnects; their own disconnect sees NULL intfdata
     * and returns above. */
    for (i = 3; i <= 4; i++) {
        struct usb_interface *aux = usb_ifnum_to_if(interface_to_usbdev(interface), i);

        if (aux && aux->dev.driver == &zg01_driver.driver)
            usb_driver_release_interface(&zg01_driver, aux);
    }

    if (atomic_xchg(&dev->disconnected, 1))
        return;
    atomic_set(&dev->disconnecting, 1);

    /* Block submissions and join late arming before cancelling quiesce. */
    zg01_stop_all_chains(dev);
    zg01_drain_all_chains(dev);
    flush_workqueue(zg01_period_wq);
    for (i = 0; i < ZG01_N_STREAMS; i++) {
        flush_work(&dev->streams[i].period_work);
        flush_work(&dev->streams[i].xrun_work);
    }

    /* Mark chains unallocated under state_mutex so a
     * concurrent trigger/chain_start (blocked on the mutex) sees the
     * teardown and bails with -ENODEV instead of submitting URBs we
     * are about to free. */
    mutex_lock(&dev->state_mutex);
    dev->out_chain.allocated = false;
    dev->in_chain.allocated = false;
    mutex_unlock(&dev->state_mutex);

    /* Firmware 1.50 wedges when hot reload kills streaming URBs without
     * a session close: the next probe fails (-71) or the device resets
     * its own USB stack (observed twice, clears only on power cycle).
     * Windows ends every session by returning BOTH streaming interfaces
     * to alt 0 (usbmon frames 104307/104331). Send the same stops here;
     * failures are logged - a physically absent device ignores them. */
    ret = usb_set_interface(interface_to_usbdev(interface), 1, 0);
    if (ret < 0)
        dev_info(&interface->dev, "disconnect stop interface 1: %d\n", ret);
    ret = usb_set_interface(interface_to_usbdev(interface), 2, 0);
    if (ret < 0)
        dev_info(&interface->dev, "disconnect stop interface 2: %d\n", ret);

    /* Free URBs and buffers for real. */
    for (i = 0; i < MAX_URBS; i++) {
        if (dev->out_chain.urbs[i]) {
            usb_kill_urb(dev->out_chain.urbs[i]);
            usb_free_urb(dev->out_chain.urbs[i]);
            dev->out_chain.urbs[i] = NULL;
        }
        kfree(dev->out_chain.bufs[i]);
        dev->out_chain.bufs[i] = NULL;
        if (dev->in_chain.urbs[i]) {
            usb_kill_urb(dev->in_chain.urbs[i]);
            usb_free_urb(dev->in_chain.urbs[i]);
            dev->in_chain.urbs[i] = NULL;
        }
        kfree(dev->in_chain.bufs[i]);
        dev->in_chain.bufs[i] = NULL;
    }

    /* Asynchronous teardown: does not hang the USB hub thread when
     * PipeWire still holds the device open. */
    snd_card_disconnect(dev->card);
    snd_card_free_when_closed(dev->card);
}

static int zg01_suspend(struct usb_interface *intf, pm_message_t message)
{
    struct zg01_dev *dev = usb_get_intfdata(intf);

    if (!dev)
        return 0;

    zg01_suspend_pcm(dev);
    zg01_stop_all_chains(dev);
    zg01_drain_all_chains(dev);
    return 0;
}

static int zg01_resume(struct usb_interface *intf)
{
    /* reset_resume handles the real reinit; plain resume is a no-op
     * beyond the stream-state reset. */
    struct zg01_dev *dev = usb_get_intfdata(intf);

    if (dev)
        zg01_pm_reset_streams(dev);
    return 0;
}

/*
 * The ZG01 firmware resets itself across suspend, so without
 * reset_resume the core runs disconnect+probe and userspace sees
 * duplicate nodes.  Keep the interfaces bound and the card objects.
 */
static int zg01_reset_resume(struct usb_interface *intf)
{
    struct zg01_dev *dev = usb_get_intfdata(intf);

    if (!dev)
        return 0;

    zg01_pm_reset_streams(dev);

    /* Device-global reinit: vendor handshake + interfaces to alt 0. */
    zg01_init_control(dev);
    usb_set_interface(dev->udev, 1, 0);
    usb_set_interface(dev->udev, 2, 0);
    return 0;
}

static int __init zg01_init(void)
{
    int ret;

    /*
     * Name an explicit CPU binding where the kernel supports it. Recent
     * kernels warn when a workqueue sets neither WQ_PERCPU nor WQ_UNBOUND,
     * and WQ_PERCPU keeps the behaviour these queues have always had;
     * ZG01_WQ_PERCPU is 0 on kernels older than v6.17, whose alloc_workqueue
     * binds to the local CPU implicitly.
     */
    zg01_cleanup_wq = alloc_workqueue("zg01-cleanup",
                                      WQ_MEM_RECLAIM | ZG01_WQ_PERCPU, 0);
    if (!zg01_cleanup_wq)
        return -ENOMEM;

    zg01_period_wq = alloc_workqueue("zg01-period",
                                     WQ_MEM_RECLAIM | ZG01_WQ_PERCPU, 0);
    if (!zg01_period_wq) {
        destroy_workqueue(zg01_cleanup_wq);
        zg01_cleanup_wq = NULL;
        return -ENOMEM;
    }

    ret = usb_register(&zg01_driver);
    if (ret) {
        destroy_workqueue(zg01_period_wq);
        destroy_workqueue(zg01_cleanup_wq);
        zg01_period_wq = NULL;
        zg01_cleanup_wq = NULL;
        return ret;
    }

    return 0;
}

static void __exit zg01_exit(void)
{
    usb_deregister(&zg01_driver);
    destroy_workqueue(zg01_period_wq);
    destroy_workqueue(zg01_cleanup_wq);
}

module_init(zg01_init);
module_exit(zg01_exit);

MODULE_AUTHOR("Yamaha ZG01 Driver Contributors");
MODULE_DESCRIPTION("Yamaha ZG01 USB Audio Driver");
MODULE_LICENSE("GPL");
