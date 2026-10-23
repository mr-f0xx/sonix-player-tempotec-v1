# Live visualization (development)

The player now offers **Off**, **Spectrum** (16 bars) and **Live wave**.
This is implemented in the source; it is not a published firmware release and
has not yet been validated on a physical TempoTec V1.

## Controls

- **Settings → Music → Display options → Live visualization**.
- Or **hold the artwork / visualization** in Now Playing for the mode menu.
- The selection is saved as `[screen] player_visualization`: `0` Off (default),
  `1` Spectrum, `2` Live wave. Invalid saved values fall back to Off.
- Enabling either visualization uses the Standard player's controls, replacing
  artwork. Your chosen layout and lyrics preference are preserved; Off restores
  them. The visualization uses the player's accent colour.

These are **live audio displays**, independent of the existing whole-track
Waveform layout (a cached overview of a track, not a live analyzer).

## Playback comes first

- The playback thread copies at most **512 first-channel PCM samples per tap**
  into a fixed mailbox, using **try-lock only**. A busy mailbox drops the picture,
  never waits for the UI. No allocations, file reads, extra decoding or FFT in
  that thread. Off is a lock-free flag check.
- UI processing is capped at roughly **15 Hz**; actual updates can be lower
  because decoder writes are relatively large. No backlog of animation frames.
- Spectrum uses a **512-point, Hann-windowed, scaled integer FFT** and fixed
  logarithmic frequency bands (60 Hz–20 kHz, limited by sample rate / FFT
  resolution). It is a lightweight visual effect, not a calibrated instrument;
  at high sample rates the lowest bands cannot be resolved and remain empty.
- Live wave draws **96 sample points**, skipping the FFT entirely.
- Pausing/stopping disables capture and clears the display. Closing the player
  or turning the screen off immediately disables capture. There is no FFT or
  repeated repaint in those states; the UI gate polls slowly while inactive.
- An output write recovery suspends capture for approximately **three seconds**
  of PCM playback. Pictures expire after 250 ms without fresh samples, including
  during cooldown or buffering. Recovery remains the audio engine's job.
- This only observes samples; it **does not change audio samples or routing**.

## Supported sources and limits

The tap is in the WAV and decoded-file PCM playback paths (16-/32-bit integer
PCM), including cached/downloaded tracks that use those paths. It does **not**
currently tap external-input / streaming-output paths (radio, AirPlay,
Bluetooth receiver or USB DAC mode). Native DSD and DoP bypass it. Those sources
show an idle baseline, not fabricated motion. DSD converted to PCM through the
normal decoder path can be visualized.

The first channel is used deliberately to avoid anti-phase stereo cancelling
the picture and to keep the audio-thread cost bounded. Visuals reflect decoded
blocks, not a sample-accurate DAC clock; output buffering/Bluetooth latency may
lead to some visual/audio offset.

## Preview

The GIFs use the **production renderer and analyzer**, driven by deterministic
synthetic PCM, in a representative 240×320 player shell enlarged 2×. The shell,
labels and transport are a test fixture, **not a full-player or hardware capture**.

[Spectrum animation](../screenshots/visualization/spectrum.gif) ·
[Live waveform animation](../screenshots/visualization/waveform.gif)

## Checks and regeneration

```sh
make -C sonix-player/tests visualizer sanitize-visualizer ui-visualization
mkdir -p sonix-player/tests/.build/visualization-frames
SONIX_VISUALIZATION_FRAMES=sonix-player/tests/.build/visualization-frames \
  sonix-player/tests/.build/test_visualization
```

The optional PPM output is generated/ignored. The DSP checks exercise known
tones and levels, silence, anti-phase stereo, 16-/32-bit samples, invalid inputs,
partial blocks and format changes, disable/discard, throttling, recovery
cooldown, extreme rates/amplitudes, and concurrent feed/read/disable. The UI test
renders both modes and checks the idle, hidden and destruction paths.

Before release, test on a V1 with MP3/FLAC/WAV, high-rate PCM and native DSD;
exercise switching/Off, layout restoration, seek/skip, pause/resume, screen
blank/wake and battery use. Monitor audio health/write recoveries while decoding
and downloading. **Host tests do not establish glitch-free hardware playback.**
