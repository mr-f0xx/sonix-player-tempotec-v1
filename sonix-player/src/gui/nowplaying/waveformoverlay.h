#ifndef WAVEFORMOVERLAY_H
#define WAVEFORMOVERLAY_H

// A smaller favourite disc on the V1 sleeve; its button remains larger so it
// is still easy to tap. The star bitmap is 32 pixels at scale 256.
#define WAVEFORM_FAV_COMPACT_SIZE 30
#define WAVEFORM_FAV_COMPACT_SCALE 176

// Both pills must leave room for the favourite: the lower pill may be hidden,
// and with larger fonts even the upper pill can reach the disc vertically.
static inline int waveform_pill_text_room(int art_width, int inset, int fav_size, int pill_pad, int gap) {
	int room = art_width - 2 * inset - fav_size - 2 * pill_pad - gap;
	return room > 0 ? room : 1;
}

#endif
