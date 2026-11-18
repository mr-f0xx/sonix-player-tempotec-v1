# Tape Deck

Enable **Settings → Plugins → Tools → Tape Deck**, restart when prompted, then open **More → Tape Deck**.

## Voice memos

- **Meter** opens the input for a level check without saving or playing it back. The segmented meter shows peak dBFS, and **CLIP** lights for a second at full scale. Metering stops when you turn off the screen or leave the page.
- **Record** opens the input and starts a new memo. A writable microSD is required. Tap **Stop** to save, or leave Tape Deck / switch to the sampler to stop and save. An active recording can continue with the screen off. Each take is limited to one hour.
- Memos are **48 kHz, mono, 16-bit PCM WAV** files under `Recordings/` on the microSD. Tap a recent memo to play it in Now Playing. Use the File Manager or a card reader for the full folder and file management.
- If the input disconnects, Tape Deck tries to save what it already captured and displays an interruption notice. If card I/O fails, the deck reports a save error rather than publishing a truncated WAV. Stop before physically pulling the card.

### Which inputs work?

Tape Deck only offers recording when Linux advertises an ALSA hardware **capture** PCM (`/sys/class/sound/pcmC*D*c`). It prefers USB capture. It never opens an input just by visiting the page, enables USB host mode on its own, or changes microphone mixer settings. There is no built-in or TRRS microphone support confirmed on stock V1 1.3: its headset SAR ADC driver is for inline remote buttons, not an audio capture stream.

Use a **class-compliant USB microphone** or a **USB-C audio dongle with mic input**. A playback-only DAC or passive 3.5 mm adapter is not enough. If the accessory doesn't enumerate on its own, use the existing **Developer options → External USB DAC** host-mode setting. Follow that setting's precautions: don't connect a computer or charger while the port is forced to host, and turn it off before connecting either. A capture-only device does not replace the headphone playback route.

On a V1, `cat /proc/asound/pcm` over ADB can confirm whether an input appears (look for `capture` in the entry). A listed PCM alone doesn't guarantee that the mic is wired, unmuted, or able to stream; try **Meter** and look for movement while speaking. USB capture still needs a physical V1/dongle test. The browser and desktop emulators do not pass through microphones.

## 16-pad sampler

Open **Pads** and tap a pad to play it. The factory kit is four kicks, four
snares, four hi-hats and four claps, each with a different tuning or envelope.
It works without a microphone or a card. In **Kits**, tap a pad's numbered row
and choose **Factory sounds** to move any of the 16 sounds to that pad.

- **Loop** starts or stops a one-bar loop. The lights mark its 16 sixteenth-note
  steps. You can tap over it without changing it. **Overdub** adds your short
  hits to the current step while the loop runs; tap it again to disarm it.
  Turn **Loop** off to keep playing pads without the sequence. **Stop** cuts
  everything and ends any mix recording.
- In **Kits**, adjust tempo from 60 to 200 BPM. Use **Clear loop** for a blank
  bar, or **Demo loop** to restore the starter rhythm (pads 01, 05 and 09).
- In **Kits**, choose a numbered pad, then **Load WAV** and browse the microSD.
  This can be a drum hit, a vocal chop, an exported acapella, or a WAV recorded
  with Tape Deck. Clips up to eight seconds play as one-shots and can be added
  to the loop. Longer clips are marked **L**, stream from the card, and play
  only when tapped. Tap again to cut the clip. One long clip can play at a time;
  the other pads and the loop can play over it.
- First playback takes over from local music, but won't take over an active
  external audio stream. Backing out or changing tabs stops the sampler.

### Save an editable sequence

1. Use **Loop** and **Overdub** to record hits in the 16-step bar. In **Kits**,
   **Clear loop** gives you an empty bar to start with.
2. In **Kits**, tap **Save sequence**. This makes a new snapshot on the microSD
   in `Sequences/Sequence-YYYYMMDD-HHMMSS-NNN.seq`.
3. Use **Kits → Load sequence** and pick a timestamped save. It restores the
   tempo, 16 steps and pad assignments; return to **Pads** and tap **Loop** to
   play or continue overdubbing. Save again to keep a new version.

The sequence points to any imported WAVs by their paths on the card; it does
not contain copies of those files. Keep the samples in their original folders,
including long backing tracks. If a sample was moved or is unreadable, the
load is refused and the current beat is left alone. Unsaved changes and live
FX switches are session-local.

### Record a performance WAV

- On **Pads**, tap **Rec mix** before playing, or while a beat is already
  running. The timer shows the length of the take. It records the sampler's
  stereo output: manual pads, the loop, any WAV backing you trigger from a pad,
  and the effects. It does **not** use the microphone or capture music playing
  outside the sampler. Headphone volume does not change the recorded level.
- Tap **End rec** to save the take while continuing to play. **Stop**, leaving
  Pads, and a normal shutdown or card export also end and finalize the take.
  Screen blanking doesn't by itself stop recording. Wait for **Take saved**
  before removing the card.
- Tap **Kits → Saved takes**, then a timestamp to listen. Takes are **44.1 kHz,
  stereo, 16-bit PCM WAV** files in `Recordings/Take-YYYYMMDD-HHMMSS-NNN.wav`
  on a writable microSD. Each take can run up to one hour (about 606 MiB).
  A slow card or interrupted audio output may end the take early and save the
  continuous part. If the WAV can't be written and finalized, it isn't listed
  as saved. Use File Manager or a card reader to copy or delete takes.

### Live effects

Tap **Filter** for a darker low-pass sound, **Echo** for a quarter-second
repeat, or **Drive** for soft distortion. Tap a lit button again to bypass it.
The three switches work independently, can be stacked, and do not restart the
loop or stop recording. They affect the whole sampler mix, including backing
tracks; switching them during a take is audible in the WAV.

### Load a whole drum kit

Create a folder under `Drum Kits/` at the root of the microSD. Place the WAVs
directly in it (unzip packs first). For example:

```text
Drum Kits/
  My Kit/
    01 Kick.wav
    05 Snare.wav
    09_Hat.wav
    13-Clap.wav
    16.wav
```

Use **Kits → Load drum kit → My Kit**. Prefixes `01`–`16` map to exact pads;
unused numbers are empty. If there are no usable numbered WAVs, the first 16
valid unnumbered WAVs are loaded in filename order. **Factory kit** restores
the built-in kit. Reselect a card kit or load a saved sequence after rebooting,
removing or exporting its card.

Import supports mono or stereo **PCM WAV** (8/16/24/32-bit integer, 8–96 kHz).
Convert MP3, AAC, FLAC or floating-point WAV to PCM WAV before importing.
This is a performance sampler with one-bar overdubbing, not a full MPC project
editor: it doesn't import MPC program files, slice clips or time-stretch them.

## On-device check

1. Open Tape Deck with no input attached; both mic buttons should be disabled and the factory pads should still play.
2. Connect a compatible microphone with the port in host mode. Check that the input appears, **Meter** responds to sound, and opening or metering alone creates no file.
3. Record, turn the screen off, speak, wake and stop. Play the memo and verify the WAV from the SD card. Repeat while disconnecting the microphone mid-take; the result should be marked interrupted and remain playable.
4. Try recording with the card missing or read-only, and retry with a writable card. No WAV should appear on internal storage.
5. Copy a numbered kit and a long stereo vocal WAV to the card. Import the kit, assign the vocal to a spare pad, start the loop and tap the vocal. Re-tap to stop it without stopping the loop. Export the card and verify that it unmounts and the factory pads still work.
