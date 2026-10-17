#!/bin/sh
# Make the Flip 2 appear to a computer or tablet on its USB-C port as a USB audio interface (UAC2, stereo 48 kHz in and
# out) plus a USB MIDI port, so autojam's output can be recorded there and the computer's MIDI can reach it.
# ROCKNIX 20261001's kernel ships configfs gadgets for CDC Ethernet and MTP only: CONFIG_USB_CONFIGFS_F_UAC2 and
# CONFIG_USB_CONFIGFS_F_MIDI are not built, so this script checks first and says so. The rocknix-gtk kernel lane can
# turn them on (docs/USB.md); then this runs as is. Operator-run: it rewrites the gadget on the USB-C port.
#   tools/usb-gadget-audio.sh on|off|status
set -u
G=/sys/kernel/config/usb_gadget/autojam
UDC=$(ls /sys/class/udc 2>/dev/null | head -1)
have() { zcat /proc/config.gz 2>/dev/null | grep -q "^$1=y" || grep -q "^${2:-nothing} " /proc/modules 2>/dev/null; }
case ${1:-status} in
  status)
    echo "udc: ${UDC:-none}"; echo "role: $(cat /sys/class/usb_role/*/role 2>/dev/null | head -1)"
    have CONFIG_USB_CONFIGFS_F_UAC2 usb_f_uac2 && echo "f_uac2: available" || echo "f_uac2: NOT in this kernel (stock ROCKNIX): USB audio gadget impossible here"
    have CONFIG_USB_CONFIGFS_F_MIDI usb_f_midi && echo "f_midi: available" || echo "f_midi: NOT in this kernel"
    [ -d $G ] && echo "autojam gadget: bound to $(cat $G/UDC)" || echo "autojam gadget: off"
    echo "host-side USB audio (a USB mic or interface plugged into the Flip 2) needs nothing: snd-usb-audio is built in." ;;
  on)
    have CONFIG_USB_CONFIGFS_F_UAC2 usb_f_uac2 || { echo "this kernel has no f_uac2; see docs/USB.md" >&2; exit 1; }
    modprobe libcomposite 2>/dev/null; modprobe usb_f_uac2 2>/dev/null; modprobe usb_f_midi 2>/dev/null
    for g in /sys/kernel/config/usb_gadget/*; do [ -f "$g/UDC" ] && [ -n "$(cat "$g/UDC")" ] && { echo "" > "$g/UDC"; echo "unbound $(basename "$g")"; }; done
    mkdir -p $G && cd $G || exit 1
    echo 0x1d6b > idVendor; echo 0x0104 > idProduct; echo 0x0100 > bcdDevice; echo 0x0200 > bcdUSB
    mkdir -p strings/0x409; echo "autojam" > strings/0x409/manufacturer; echo "autojam audio+MIDI" > strings/0x409/product; echo "0001" > strings/0x409/serialnumber
    mkdir -p configs/c.1/strings/0x409; echo "audio" > configs/c.1/strings/0x409/configuration; echo 250 > configs/c.1/MaxPower
    mkdir -p functions/uac2.usb0; echo 48000 > functions/uac2.usb0/c_srate; echo 48000 > functions/uac2.usb0/p_srate; echo 3 > functions/uac2.usb0/c_chmask; echo 3 > functions/uac2.usb0/p_chmask; echo 2 > functions/uac2.usb0/c_ssize; echo 2 > functions/uac2.usb0/p_ssize
    ln -sf $G/functions/uac2.usb0 configs/c.1/ 2>/dev/null
    if have CONFIG_USB_CONFIGFS_F_MIDI usb_f_midi; then mkdir -p functions/midi.usb0; echo 1 > functions/midi.usb0/in_ports; echo 1 > functions/midi.usb0/out_ports; ln -sf $G/functions/midi.usb0 configs/c.1/ 2>/dev/null; fi
    echo device > /sys/class/usb_role/*/role 2>/dev/null
    echo "$UDC" > UDC && echo "gadget on: the computer sees a 'autojam audio+MIDI' device; a new ALSA card appears here too (aplay -l)" ;;
  off)
    [ -d $G ] || { echo "gadget is off"; exit 0; }
    echo "" > $G/UDC 2>/dev/null; rm -f $G/configs/c.1/uac2.usb0 $G/configs/c.1/midi.usb0; rmdir $G/configs/c.1/strings/0x409 $G/configs/c.1 $G/functions/* $G/strings/0x409 $G 2>/dev/null
    echo "gadget off (the CDC/MTP gadgets ROCKNIX manages come back on their own at the next plug)" ;;
  *) echo "usage: $0 on|off|status" >&2; exit 2 ;;
esac
