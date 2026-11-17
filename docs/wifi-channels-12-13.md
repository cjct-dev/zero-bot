# Wi-Fi networks on channels 12 and 13 are invisible

## Symptom

The CardputerZero lists neighbouring networks but never shows one particular network, even
though other devices in the same room see it. The missing network turns out to be on 2.4 GHz
channel 12 or 13 (common in Europe and Asia when a router picks its channel automatically).

Note that the radio is 2.4 GHz only, so a network that broadcasts only on 5 GHz can never be
seen, whatever the settings.

## Cause

Seen on the CardputerZero system image with kernel `6.18.39+rpt-rpi-v8`:

- Linux is configured for the right country, but the Wi-Fi chip's firmware stays on its default
  country, `US`, which only uses channels 1 to 11.
- The `brcmfmac` driver has no country mapping for this chip (CYW43439), so it never passes the
  configured country to the firmware. With driver debugging on it logs
  `brcmf_translate_country_code No country codes configured for device`.
- `iw reg set GB` only changes the kernel's own channel list. `iw list` then shows channels 12
  and 13 as enabled, but scans still return nothing on them.
- Editing `ccode=` in the firmware's NVRAM file does not help: the Raspberry Pi kernel strips
  that setting (`brcmfmac: intercepting ccode=...`).

## Fix

A one-property device-tree overlay tells the driver to hand the ISO country code straight to the
firmware. The source is in [`extras/zb-wifi-ccode-overlay.dts`](../extras/zb-wifi-ccode-overlay.dts).

On the device, as root:

```sh
# 1. Make sure the country is set (GB here; use your own two-letter code).
raspi-config nonint do_wifi_country GB

# 2. Build and install the overlay.
dtc -@ -I dts -O dtb -o /boot/firmware/overlays/zb-wifi-ccode.dtbo zb-wifi-ccode-overlay.dts

# 3. Try it without rebooting.
dtoverlay zb-wifi-ccode
modprobe -r brcmfmac_cyw brcmfmac && modprobe brcmfmac
iw list | grep -E '\[1[23]\]'      # should no longer say "(disabled)"
nmcli dev wifi rescan && nmcli dev wifi list

# 4. Make it permanent.
cp -a /boot/firmware/config.txt /boot/firmware/config.txt.orig
printf '\n[all]\ndtoverlay=zb-wifi-ccode\n' >> /boot/firmware/config.txt
```

If `dtc` is missing, install it with `apt install device-tree-compiler`.

## Undo

```sh
cp -a /boot/firmware/config.txt.orig /boot/firmware/config.txt
rm /boot/firmware/overlays/zb-wifi-ccode.dtbo
reboot
```

## Alternative

Set the router's 2.4 GHz radio to a fixed channel 1, 6 or 11 instead of automatic.
