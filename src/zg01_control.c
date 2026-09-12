/*
 * Yamaha ZG01 USB Audio Driver - Control Interface
 *
 * Based on analysis of Windows USB packet capture
 */

#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <sound/control.h>
#include <sound/tlv.h>

#include "zg01.h"
#include "zg01_control.h"

static bool enable_mic_monitor;
module_param(enable_mic_monitor, bool, 0444);
MODULE_PARM_DESC(enable_mic_monitor,
                "Expose experimental write-only mic_monitor_level on USB interface 1");

/* Caller serializes with state_mutex. No readback protocol is known yet. */
static int zg01_set_mic_monitor(struct zg01_dev *dev, unsigned int level)
{
    static const u8 prefix[] = {
        0x04, 0xf0, 0x43, 0x10, 0x04, 0x3e, 0x14, 0x01,
        0x04, 0x01, 0x00, 0x01, 0x04, 0x6d, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
        0x05, 0xf7, 0x00, 0x00,
    };
    u8 *buf;
    int ret, actual;

    if (level > 127)
        return -EINVAL;

    buf = kzalloc(512, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    /* Capture 70-monitor-level-20260911-152339, frame 59524: level 103.
     * Keep the observed selector and 512-byte padding unchanged. */
    memcpy(buf, prefix, sizeof(prefix));
    buf[23] = level;
    ret = usb_bulk_msg(dev->udev, usb_sndbulkpipe(dev->udev, 3),
                       buf, 512, &actual, 1000);
    kfree(buf);
    if (ret)
        return ret;
    return actual == 512 ? 0 : -EIO;
}

static ssize_t mic_monitor_level_store(struct device *device,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
    struct usb_interface *intf = to_usb_interface(device);
    struct usb_device *udev = interface_to_usbdev(intf);
    struct usb_interface *primary = usb_ifnum_to_if(udev, 1);
    struct zg01_dev *dev;
    unsigned int level;
    int ret;

    ret = kstrtouint(buf, 10, &level);
    if (ret)
        return ret;
    if (level > 127)
        return -EINVAL;

    dev = primary ? usb_get_intfdata(primary) : NULL;
    if (!dev)
        return -ENODEV;

    /* Disconnect removes this attribute and joins stores before freeing dev.
     * The driver does not enable runtime autosuspend. state_mutex serializes
     * this transfer with system suspend and the PCM control requests. */
    mutex_lock(&dev->state_mutex);
    if (atomic_read(&dev->disconnecting) || atomic_read(&dev->disconnected))
        ret = -ENODEV;
    else if (dev->suspended)
        ret = -EHOSTDOWN;
    else
        ret = zg01_set_mic_monitor(dev, level);
    mutex_unlock(&dev->state_mutex);

    /* USB completion is not a device-level acknowledgment. */
    return ret ? ret : count;
}
static DEVICE_ATTR_WO(mic_monitor_level);

int zg01_init_mic_monitor(struct usb_interface *intf)
{
    struct usb_host_interface *alt = intf->cur_altsetting;
    struct usb_interface *primary;
    int i;

    if (!enable_mic_monitor)
        return 0;
    primary = usb_ifnum_to_if(interface_to_usbdev(intf), 1);
    if (!primary)
        return -ENODEV;
    for (i = 0; i < alt->desc.bNumEndpoints; i++) {
        struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

        /* Interface 4 may not have reached device_add() during cold probe.
         * Interface 1 is already registered and owns the card lifetime. */
        if (ep->bEndpointAddress == 0x03 && usb_endpoint_is_bulk_out(ep))
            return device_create_file(&primary->dev, &dev_attr_mic_monitor_level);
    }
    return -ENODEV;
}

void zg01_remove_mic_monitor(struct usb_interface *intf)
{
    struct usb_interface *primary;

    primary = usb_ifnum_to_if(interface_to_usbdev(intf), 1);
    if (primary)
        device_remove_file(&primary->dev, &dev_attr_mic_monitor_level);
}

int zg01_init_control(struct zg01_dev *dev)
{
    int ret;
    unsigned char *buf;
    
    if (!dev || !dev->udev || !dev->interface) {
        return -ENODEV;
    }

    /* Allocate DMA-coherent buffer for USB control message */
    buf = kmalloc(256, GFP_KERNEL);
    if (!buf) {
        pr_err("zg01_control: Failed to allocate control buffer\n");
        return -ENOMEM;
    }

    /* Device initialization sequence based on USB capture */
    pr_info("zg01_control: Initializing Yamaha ZG01 device\n");

    /* Vendor-specific control request - appears to be device initialization */
    ret = usb_control_msg(dev->udev, 
                         usb_rcvctrlpipe(dev->udev, 0), 
                         7,      /* bRequest */
                         0xc0,   /* bmRequestType: vendor, device-to-host */
                         0x0000, /* wValue */
                         0,      /* wIndex */ 
                         buf, 3, /* expect 3 bytes response (80bb00) */
                         1000);
    if (ret < 0) {
        pr_err("zg01_control: ZG01 initialization request failed: %d\n", ret);
        kfree(buf);
        return ret;
    } else if (ret == 3) {
        pr_info("zg01_control: ZG01 init response: %02x%02x%02x\n", 
                buf[0], buf[1], buf[2]);
        /* Expected response should be 0x80, 0xbb, 0x00 */
        if (buf[0] == 0x80 && buf[1] == 0xbb && buf[2] == 0x00) {
            pr_info("zg01_control: ZG01 initialization successful\n");
        } else {
            pr_warn("zg01_control: Unexpected ZG01 init response\n");
        }
    } else {
        pr_debug("zg01_control: short response (%d bytes)\n", ret);
    }

    kfree(buf);

    return 0;
}
