# USB DAC / iPhone regression (#358)

DAC setup reused `android0` without writing idVendor/idProduct. After storage
setup it therefore retained the storage PID (0x0101), rather than the audio PID
(0x0004, documented alongside the stock IDs in usb.c). A newly created gadget
could instead retain configfs defaults. Set both IDs explicitly before binding,
and fail initialization if either write fails. This avoids presenting different
USB functions under the storage identity.

DAC entry also requests the existing guarded Type-C sink setup after releasing
the external-host override and reclaiming the card. It does not override an
active external DAC, pending handover, or reserved host session. Devices without
Type-C class nodes retain the existing platform-specific role arbitration.

Host-side regression checks:

```
make -C sonix-player/tests usbhost usbdac-identity usbdac-eq
```

These cover identity writes from fresh and storage states, failed descriptor
writes, guarded sink requests, USB ownership/storage/external-host regressions,
and PCM effects/DSD passthrough. They cannot establish iOS compatibility.

## Hardware confirmation still required

1. On a V1 with the updated player, enter DAC mode, then connect the reported
   iPhone with the same USB-C data cable. Start audio; check output selection,
   playback through the V1 and the displayed sample rate.
2. Repeat with the cable attached before entering DAC mode; then disconnect and
   reconnect while staying in DAC mode.
3. Leave DAC mode, use storage, and re-enter DAC mode. Verify storage advertises
   32bb:0101 and audio advertises 32bb:0004 (a Linux host's lsusb is useful).
4. Repeat audio/storage switching on a desktop host. Verify external USB-DAC
   output still works after exiting input DAC mode.
5. If iOS still fails, collect the player log, UDC state, actual USB descriptors
   and role-node values. The reported failure is not hardware-confirmed fixed;
   further driver/descriptor compatibility changes may be necessary. Audio
   format, feedback and power descriptors are deliberately unchanged here.
