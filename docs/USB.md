# USB on the Flip 2 for autojam

Checked on the 12 GB test card, ROCKNIX 20261001, kernel 7.2.0 (2026-10-17):

| kernel option | state | meaning for autojam |
|---|---|---|
| `CONFIG_SND_USB_AUDIO=y` | built in | USB mics and audio interfaces plugged into the Flip 2 work (PipeWire sources/sinks) |
| `CONFIG_SND_RAWMIDI=y` | built in | USB MIDI devices appear as `/dev/snd/midiC*D*`; autojam uses them directly |
| `CONFIG_SND_SEQUENCER` | not set | no ALSA sequencer: raw MIDI only (fine) |
| `CONFIG_USB_DWC3_DUAL_ROLE=y`, `CONFIG_USB_ROLE_SWITCH=y` | built in | the USB-C port switches between host and device (`/sys/class/usb_role/a600000.usb-role-switch/role`) |
| `CONFIG_USB_GADGET=y` + configfs | built in | ROCKNIX configures `cdc` (USB networking) and `mtp` gadgets |
| `CONFIG_USB_CONFIGFS_F_UAC2`, `F_UAC1`, `F_MIDI`, `F_MIDI2` | **not set** | the Flip 2 cannot present itself as a USB sound card or MIDI port on stock ROCKNIX |

## Host mode: what works today

Plug a USB-C hub or OTG adapter with a USB audio interface, microphone or MIDI keyboard. `pactl list short
sources` shows the interface's input (autojam prefers a source whose name contains `usb`), `aplay -l` a second
card, `ls /dev/snd/midi*` the MIDI port. autojam's **USB MIDI ›** button cycles the raw MIDI ports; the band's
notes go out on channels 1 (bass), 2 (keys), 3 (lead) and 10 (drums, GM notes 36/38/42/46/37/39), with MIDI
clock at 24 ppqn plus Start/Stop, so a drum machine or synth follows the room's tempo. Notes played in on the
same port are added to the listener's chroma as held chord tones; CC 1 (mod wheel) steps the band's energy
in quarters.

Bus power: a hub with its own supply is the safe choice; the stock device tree does not advertise 5 V out on
the port (ETK's VBUS delta does, see `mercurious/etk`).

## Device mode: the kernel delta for ROCKNIX-GTK

To make the Flip 2 a class-compliant USB audio interface (stereo 48 kHz in and out) and a USB MIDI port for a
laptop or tablet, the kernel needs the gadget functions. In the `rocknix-gtk` kernel lane
(`patches-7.2/`, built by `~/etk/tools/forge/lane_kernel.sh` on etk-cloud), add to the config fragment:

```
CONFIG_USB_CONFIGFS_F_UAC2=y
CONFIG_USB_CONFIGFS_F_MIDI=y
CONFIG_SND_USB_AUDIO_MIDI_V2=y     # optional: MIDI 2.0 capable hosts
```

These are kernel-image options whose code lives in the image (`usb_f_uac2`, `usb_f_midi`, `libcomposite` are
already built in with `CONFIG_USB_GADGET=y`), so they respect the module-ABI law of that repo: `uname` and the
stock `/lib/modules` stay as they are. Validate like any GTK kernel (cold boot, stock entry one pick away).

With such a kernel booted, on the device:

```sh
/storage/apps/autojam/tools/usb-gadget-audio.sh status   # f_uac2: available
/storage/apps/autojam/tools/usb-gadget-audio.sh on       # the laptop sees "autojam audio+MIDI"; a new ALSA card appears here
```

The new ALSA card (`UAC2Gadget`) shows up in PipeWire as a sink and a source: pick it with autojam's
**Output ›** to send the band to the laptop, and the laptop's audio comes back as a source the mic picker
prefers (its name contains `usb`). The gadget script unbinds ROCKNIX's `cdc`/`mtp` gadget while it is on and
`off` removes it again; USB networking returns at the next plug. It writes the gadget on the port, so it is a
tool for the operator at the device, not something the app does on its own.
