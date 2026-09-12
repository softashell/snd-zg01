# Experimental mic monitor level

Load the local module with `enable_mic_monitor=1` to expose a write-only
`mic_monitor_level` sysfs attribute on USB interface 1. The default is off.
Interface 4 still carries the bulk transfer. The attribute uses interface 1
because interface 4 may not yet exist in sysfs during device enumeration.
This does not add an ALSA mixer control or change any level at module load.

The control accepts decimal raw values from 0 through 127. It sends one
512-byte bulk transfer to endpoint 0x03. Invalid values and short or failed
transfers return an error. The driver does not retry or replay the value on
resume, reset, or reconnect.

## Trial

Stop audio clients and unload the old module using your normal reload procedure.
From the repository root, load the newly built module:

```bash
sudo insmod ./snd-zg01.ko enable_mic_monitor=1
```

Resolve the attribute without assuming a USB bus address. This Bash block
refuses to write if it finds zero or multiple controls:

```bash
shopt -s nullglob
controls=(/sys/bus/usb/drivers/zg01_usb/*/mic_monitor_level)
if (( ${#controls[@]} == 1 )); then
    printf '%s\n' 1 | sudo tee "${controls[0]}" >/dev/null
else
    printf 'Expected one monitor control, found %s\n' "${#controls[@]}" >&2
fi
```

Start with headphones at a low physical volume and increase the raw level
gradually while speaking, without software loopback. Do not start with 127.
The user confirmed clear volume increases at 50, 100, and 120. The difference
between 1 and 2 was too small to notice.

The Windows metadata lists 103 as the default raw value. This does not establish
your previous setting. Note the previous Windows setting before testing, so you
can restore it. Settings may persist after unloading or power cycling.

If the attribute is absent, check that the loaded module has
`enable_mic_monitor=Y` in `/sys/module/snd_zg01/parameters/enable_mic_monitor`
and owns interface 4. The driver does not detach another driver to take it.

Hardware ear testing confirms that standalone Linux writes change mic monitor
volume without the Windows controller's startup sequence in the tested setup.
There is no readback. A successful write means USB transfer completion only,
not a confirmed device-level acknowledgment. The UI/dB mapping and negative
wire values remain unverified.

## Trace evidence

Source: `70-monitor-level-20260911-152339.pcapng`, stored in the project's
sibling `captures/` directory. Bulk OUT submissions on bus 3, device 89:

| Frame | Value | Evidence |
|---|---:|---|
| 36547 | 1 | Successful completion at 36549 |
| 59524 | 103 | Exact template used by the driver |
| 63794 | 127 | Successful completion at 63795 |

USB-MIDI payload prefix, with `VV` as the level:

```text
04 f0 43 10 04 3e 14 01 04 01 00 01 04 6d 00 00
04 00 00 00 04 00 00 VV 05 f7 00 00
```

The remaining bytes are zero up to the observed 512-byte transfer length.
The selector contains `01 6d`, which encodes metadata ID 237 in base 128.
The capture contains 161 writes with this fixed selector and changing values.
The hardware ear test confirms the `MicMonitorLevel` assignment. The recording
lacks precise UI-value markers, so the Windows UI mapping remains unverified.

The trace also contains five-byte encodings consistent with signed -1 and -2.
Their meaning is unknown. The driver deliberately rejects negative values.
