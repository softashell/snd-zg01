#ifndef ZG01_CONTROL_H
#define ZG01_CONTROL_H

struct zg01_dev;
struct usb_interface;

int zg01_init_mic_monitor(struct usb_interface *intf);
void zg01_remove_mic_monitor(struct usb_interface *intf);

struct zg01_control {
	struct zg01_dev *zg01;

	bool phono_mic_switch;
};

int zg01_init_control(struct zg01_dev *zg01);

#endif /* ZG01_CONTROL_H */