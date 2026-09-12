# Yamaha ZG01 Linux kernel driver

An ALSA driver for the Yamaha ZG01 USB audio interface (VID 0x0499, PID
0x1513). The device uses a vendor-specific protocol, so this driver reverse
engineers it from USB captures instead of using the generic `snd-usb-audio`
class driver.

## How the driver maps the device

The ZG01 carries both playback channels on one isochronous endpoint. The
driver mixes them into the shared URB stream. It exposes one ALSA card with
three PCM devices:

| PCM device | Name | Direction | Rates | Packet |
|---|---|---|---|---|
| 0 | Game Out | playback | 48 kHz | 200–280 B: 5–7 frames x 40 B, 240 B nominal |
| 1 | Voice Out | playback | 48 kHz | shared EP 0x01 with Game Out |
| 2 | Voice In | capture | 48 kHz | 108 B nominal: 8 B header, 5-7 frames x 16 B, 4 B trailer |

All channels run S32_LE stereo. Both sinks work at the same time. Both
packages install a UCM profile, so PipeWire shows the devices as separate
sinks named Game Out, Voice Out, and Voice In. Without it, userspace falls
back to the single generic stereo profile and only Game Out is exposed.

A single OUT chain serves both playback PCMs. Each consumer supplies its own
frame slots, with silence in inactive slots. Valid IN plans supply variable
framing when IN runs. Playback-only free-run uses nominal six-frame packets.
Both-chain keepalive can preserve the live transport across playback close.

## Install

### Arch Linux, CachyOS, Omarchy

Install the headers matching your kernel (stock kernels use `linux-headers`),
then build and install the package. DKMS hooks build the module for every
installed kernel with matching headers; without them DKMS registers nothing,
so the package warns at install time when the running kernel has no headers:

```bash
sudo pacman -S linux-headers      # or linux-lts-headers / linux-zen-headers
cd packaging/arch
makepkg --cleanbuild
sudo pacman -U snd-zg01-dkms-git-*.pkg.tar.zst
```

The package installs the modules-load.d entry and the UCM profile. Reboot
after install so `snd-zg01` registers before the generic Yamaha match claims
the device. See `packaging/arch/README.md` for verification and rollback.

### Debian, Ubuntu

Add the signed APT repository, then install:

```bash
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://raw.githubusercontent.com/bsauvajon/snd-zg01/main/apt/snd-zg01.asc \
  | sudo tee /etc/apt/keyrings/snd-zg01.asc >/dev/null
curl -fsSL https://raw.githubusercontent.com/bsauvajon/snd-zg01/main/apt/snd-zg01.sources \
  | sudo tee /etc/apt/sources.list.d/snd-zg01.sources >/dev/null
sudo apt update
sudo apt install snd-zg01-dkms
```

Or download the `.deb` from the latest release and install it manually:

```bash
sudo dpkg -i snd-zg01-dkms_*.deb
sudo apt-get install -f   # only if dependencies are missing
```

### From source

The module builds against kernel headers. Clang-built kernels need the LLVM
front end; the Makefile reads `CONFIG_CC_IS_CLANG` from the target kernel and
sets it. A user-supplied `LLVM=` value wins over the auto-detection:

```bash
make
sudo modprobe snd-zg01
```

## Verify

With the device connected:

```bash
cat /proc/asound/cards      # one card: zg01
lsmod | grep snd_zg01
journalctl -b -k --grep zg01
```

The card offers three PCM devices: `hw:N,0` Game Out, `hw:N,1` Voice Out,
`hw:N,2` Voice In. With the UCM profile installed, PipeWire names them the
same way.

```bash
# Continuous stereo tone avoids channel-switching clicks.
ffmpeg -y -f lavfi -i "sine=frequency=440:duration=10" -ar 48000 -ac 2 -c:a pcm_s32le /tmp/zg01-tone.wav
aplay -D hw:zg01,0 /tmp/zg01-tone.wav
# Voice In
arecord -D hw:zg01,2 -f S32_LE -r 48000 -c 2 -d 5 test.wav
```

Do not treat all startup errors as benign. Inspect per-packet statuses and
XRUN deltas. See [startup recovery](docs/STARTUP_RECOVERY.md) for keepalive,
initialization checks, and the optional transient-IN grace experiment.

## DKMS

Both packages install the source to DKMS. DKMS rebuilds the module on kernel
updates. Remove the package with `pacman -R snd-zg01-dkms-git` or
`apt remove snd-zg01-dkms`; the hooks unload the module and clean `/usr/src`
and `/var/lib/dkms`.

Upgrades from the old split-driver packages (modules `zg01_usb`,
`zg01_pcm`, `zg01_control`, `zg01_usb_discovery`) unload those modules during
install. If a pre-2026 package left broken DKMS state, remove
`/var/lib/dkms/snd-zg01` and `/usr/src/snd-zg01-*`, then run `depmod -a`.

## Troubleshooting

- Device missing: check `lsusb | grep 0499:1513`, then
  `sudo modprobe snd-zg01` and read `journalctl -b -k --grep zg01`.
- Wrong device claimed the card: check `snd-zg01` loads before
  `snd-usb-audio` (modules-load.d entry).
- No audio on one sink: Game Out and Voice Out share one endpoint; check
  the other sink is not open in exclusive mode.
- Rate problems: playback is 48 kHz only. Voice In is 48 kHz only too.
  Use `plughw:` for format conversion.

## Documentation

- `docs/MIC_MONITOR.md`: opt-in mic monitor level control and trace evidence
- `docs/PROTOCOL_CAPTURE.md`: capture workflow for knobs, buttons, routing
- `docs/INITIALIZATION_ANALYSIS.md`: device USB topology and packet formats
- `packaging/arch/README.md`: Arch packaging, verification, rollback

## Scope and status

Working: Game Out + Voice Out simultaneous playback, Voice In capture,
suspend/resume, replug, single module, single card. Not implemented: MIDI,
other sample rates.

Experimental out-of-tree driver. Kernel updates can break the build; report
issues with `dmesg` output.

## Contributing

Issues and pull requests are welcome. For protocol work, follow
`docs/PROTOCOL_CAPTURE.md` and keep raw captures out of Git.

## License

GPL-2.0
