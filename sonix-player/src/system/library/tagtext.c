#include "tagtext.h"

#include "jisx0208.h"

#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Everything here reads bytes off a card and writes UTF-8. A tag is a claim
// about its own contents, not a fact: every loop stops at the length it was
// given and every output buffer is NUL-terminated, whatever the bytes do.
//
// The order the encodings are tried in is the order the evidence is strongest
// in, and it lives in tagtext_guess() at the end of the file.
// ---------------------------------------------------------------------------

static void utf16_to_utf8(const uint8_t *data, size_t len, bool big_endian, char *out, size_t out_size) {
	size_t oi = 0;
	size_t i = 0;
	while (i + 1 < len && oi + 4 < out_size) {
		uint32_t unit = big_endian ? ((uint32_t)data[i] << 8 | data[i + 1]) : ((uint32_t)data[i + 1] << 8 | data[i]);
		i += 2;
		if (unit == 0)
			break;

		uint32_t cp = unit;
		if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < len) {
			uint32_t unit2 = big_endian ? ((uint32_t)data[i] << 8 | data[i + 1]) : ((uint32_t)data[i + 1] << 8 | data[i]);
			if (unit2 >= 0xDC00 && unit2 <= 0xDFFF) {
				cp = 0x10000 + ((unit - 0xD800) << 10) + (unit2 - 0xDC00);
				i += 2;
			}
		}

		if (cp < 0x80) {
			out[oi++] = (char)cp;
		} else if (cp < 0x800) {
			out[oi++] = (char)(0xC0 | (cp >> 6));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out[oi++] = (char)(0xE0 | (cp >> 12));
			out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		} else {
			out[oi++] = (char)(0xF0 | (cp >> 18));
			out[oi++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[oi] = '\0';
}

// ---------------------------------------------------------------------------
// UTF-8 where it was not declared
//
// Text in a field that says Latin-1 (an ID3v2 frame with encoding 0x00, an
// ID3v1 tag) or says nothing (RIFF INFO, the text of a .dff) is quite often
// UTF-8: taggers write what the system gives them. Read as Latin-1, every
// accented letter comes out as two wrong ones. Well-formed UTF-8 with at least
// one sequence of two or more bytes is taken as UTF-8; real Latin-1 almost
// never is, since there the byte after an accented letter is another letter,
// not a continuation.
// ---------------------------------------------------------------------------



// The length of the UTF-8 sequence opened by `c`, or 0 when `c` cannot open
// one. Not a validity check -- text_is_utf8() adds the continuation bytes.
size_t tagtext_utf8_length(uint8_t c) {
	if (c < 0x80) {
		return 1;
	}
	if (c >= 0xC2 && c <= 0xDF) {
		return 2;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		return 4;
	}
	return 0;
}

// Whether `len` bytes, up to the first NUL, are well-formed UTF-8. Plain ASCII
// is UTF-8 too.
bool tagtext_is_utf8(const uint8_t *p, size_t len) {
	size_t i = 0;
	while (i < len && p[i]) {
		size_t follow = tagtext_utf8_length(p[i]);
		if (follow == 0) {
			return false;
		}
		i++;
		for (size_t k = 1; k < follow; k++, i++) {
			if (i >= len || (p[i] & 0xC0) != 0x80) {
				return false;
			}
		}
	}
	return true;
}

// Well-formed UTF-8 that is not plain ASCII.
static bool text_is_wide_utf8(const uint8_t *p, size_t len) {
	bool wide = false;
	for (size_t i = 0; i < len && p[i] && !wide; i++) {
		wide = p[i] >= 0x80;
	}
	return wide && tagtext_is_utf8(p, len);
}

// ---------------------------------------------------------------------------
// Windows-1251 in a Latin-1 frame
//
// An ID3v2 frame that declares encoding 0x00 says its bytes are ISO-8859-1, and
// most taggers tell the truth. Russian ones often do not: they write
// Windows-1251 and leave the encoding byte at zero, as does every ID3v1 tag,
// which has no encoding byte at all. Read as Latin-1, "Алиса" comes out as
// "Àëèñà" -- still in the database and still sorting, but under A, in
// characters nobody is looking for.
//
// The two encodings cannot be told apart by the bytes alone, so this goes by
// how they are arranged. A Latin-1 name spends its high bytes one at a time --
// the e in "Café", the u in "Müller" -- with letters either side. Cyrillic
// written as 1251 is high bytes all the way through a word. So a run of two or
// more Cyrillic-range letters with no ASCII letter or digit touching either end
// is 1251; anything else is left as Latin-1.
// ---------------------------------------------------------------------------

// The half of the code page that is not simply А-я: punctuation, the Ukrainian
// and Belarusian letters, and the currency signs. 0 means "not in the page".
static const uint16_t CP1251_HIGH[64] = {
	0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, // 80-87
	0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F, // 88-8F
	0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, // 90-97
	0x0000, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, // 98-9F
	0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, // A0-A7
	0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, // A8-AF
	0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, // B0-B7
	0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, // B8-BF
};

// One byte of the code page as a Unicode code point.
static uint32_t cp1251_code(uint8_t c) {
	if (c < 0x80) {
		return c;
	}
	if (c >= 0xC0) {
		// А-Я then а-я, in one contiguous stretch each.
		return (c < 0xE0) ? (uint32_t)(0x0410 + (c - 0xC0)) : (uint32_t)(0x0430 + (c - 0xE0));
	}
	uint16_t mapped = CP1251_HIGH[c - 0x80];
	return mapped ? mapped : (uint32_t)c;
}

// A Cyrillic LETTER in the code page -- what a word is made of, as opposed to
// the quotes and the currency signs that share the upper half.
static bool cp1251_is_letter(uint8_t c) { return c >= 0xC0 || c == 0xA8 || c == 0xB8; }

static bool ascii_alnum(uint8_t c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// A whole word of high bytes, with no ASCII letter touching either end, is the
// tell. An accent inside a Latin word always has letters beside it -- the u in
// "Müller", the o in "Motörhead", the three umlauts in "ÄÖÜX" -- while a
// Cyrillic word written as 1251 runs from one space to the next without an
// ASCII letter in sight. Two letters is enough of a word; the high bytes that
// are punctuation in both code pages are neither evidence nor objection.
static bool looks_like_cp1251(const uint8_t *data, size_t len) {
	size_t i = 0;
	while (i < len && data[i]) {
		if (!cp1251_is_letter(data[i])) {
			i++;
			continue;
		}

		size_t start = i;
		while (i < len && data[i] && cp1251_is_letter(data[i])) {
			i++;
		}

		bool free_left = start == 0 || !ascii_alnum(data[start - 1]);
		bool free_right = i >= len || data[i] == 0 || !ascii_alnum(data[i]);
		if (i - start >= 2 && free_left && free_right) {
			return true;
		}
	}
	return false;
}

// The same bytes as UTF-8, read through the code page.
static void cp1251_to_utf8(const uint8_t *data, size_t len, char *out, size_t out_size) {
	size_t oi = 0;
	for (size_t i = 0; i < len && data[i]; i++) {
		uint32_t code = cp1251_code(data[i]);
		if (code < 0x80) {
			if (oi + 1 >= out_size) {
				break;
			}
			out[oi++] = (char)code;
		} else if (code < 0x800) {
			if (oi + 2 >= out_size) {
				break;
			}
			out[oi++] = (char)(0xC0 | (code >> 6));
			out[oi++] = (char)(0x80 | (code & 0x3F));
		} else {
			if (oi + 3 >= out_size) {
				break;
			}
			out[oi++] = (char)(0xE0 | (code >> 12));
			out[oi++] = (char)(0x80 | ((code >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (code & 0x3F));
		}
	}
	out[oi] = '\0';
}

// ---------------------------------------------------------------------------
// Japanese in a Latin-1 frame
//
// The same frame, from a tagger on a Japanese machine: the encoding byte stays
// at 0x00, as the spec says it may for Latin-1, but the bytes are the system
// code page -- Shift-JIS on Windows, EUC-JP where the tagger was a Unix tool.
// Read as Latin-1, "宇多田ヒカル" comes out as something like "© ä Þ Ý - é",
// and a whole library is filed under the wrong letters.
//
// Three encodings, then, for the same byte 0x00, and the bytes alone have to
// tell them apart -- a Japanese name, a Russian one and a French one all sit
// above 0x7F. What separates them is where:
//
//   * Latin-1 accented letters live in 0xA0..0xFF, one byte each, next to the
//     ASCII letters of the word they belong to. Bytes 0x80..0x9F are C1
//     controls there: no tagger writes them, and no font draws them.
//   * Windows-1251 Cyrillic lives in 0xC0..0xFF, several in a row, with no
//     ASCII letter inside a word.
//   * Shift-JIS spends a lead byte in 0x81..0x9F or 0xE0..0xFC on most of its
//     characters. Halfwidth katakana are single bytes in 0xA1..0xDF, and
//     kana and kanji make up nearly every character of a title.
//   * EUC-JP is 0xA1..0xFE twice for every character, kana in rows 4 and 5.
//
// So Shift-JIS is the one that can be recognised outright: a lead byte in
// 0x81..0x9F is proof of it, since Latin-1 has nothing to draw there and
// Cyrillic never goes below 0xC0. The rest is decided by what comes out --
// a text that decodes cleanly into kana and kanji is not a French title.
//
// The table is JIS X 0208 (jisx0208.c): both encodings address the same 94 x
// 94 grid, so one lookup serves both. Shift-JIS puts the row in six bits of
// space between the two lead-byte ranges, which is the arithmetic in
// sjis_row_cell().
// ---------------------------------------------------------------------------

#include "jisx0208.h"

// The character at a JIS X 0208 row and cell, or NULL for an empty one.
static const char *jis_char(uint32_t row, uint32_t cell) {
	if (row < JIS_GRID_FIRST || row > JIS_GRID_LAST || cell < JIS_GRID_FIRST || cell > JIS_GRID_LAST) {
		return NULL;
	}
	uint16_t offset = JIS_OFFSETS[(row - JIS_GRID_FIRST) * JIS_GRID_ROWS + (cell - JIS_GRID_FIRST)];
	return offset ? (const char *)JIS_POOL + offset : NULL;
}

// The row and cell a Shift-JIS pair addresses. The first byte picks two rows
// and the second says which side of the pair; the second byte's range also
// carries the cell, with 0x80..0x9E shifted by one because the range does not
// follow the cell numbering.
static bool sjis_row_cell(uint8_t lead, uint8_t trail, uint32_t *row, uint32_t *cell) {
	uint32_t r;
	if (lead >= 0x81 && lead <= 0x9F) {
		r = (uint32_t)(lead - 0x81) * 2 + 0x21;
	} else if (lead >= 0xE0 && lead <= 0xFC) {
		r = (uint32_t)(lead - 0xC1) * 2 + 0x21;
	} else {
		return false;
	}
	if (trail >= 0x40 && trail <= 0x7E) {
		*cell = (uint32_t)(trail - 0x1F);
	} else if (trail >= 0x80 && trail <= 0x9E) {
		*cell = (uint32_t)(trail - 0x20);
	} else if (trail >= 0x9F && trail <= 0xFC) {
		r++;
		*cell = (uint32_t)(trail - 0x7E);
	} else {
		return false;
	}
	if (r < JIS_GRID_FIRST || r > JIS_GRID_LAST) {
		return false;
	}
	*row = r;
	return true;
}

// What a decoded text is made of, for deciding whether it is Japanese.
typedef struct {
	int kana;	// hiragana, katakana, and the marks that belong to them
	int kanji;	// the CJK ideographs, kana iteration marks, IDEOGRAPHIC MARK
	int other;	// anything else above ASCII
	int ascii;	// ASCII letters
	int total;	// every character
} jp_mix_t;

// Classifies one UTF-8 character of the pool (never longer than four bytes).
static void jp_count(jp_mix_t *mix, const char *text) {
	uint32_t code;
	if ((text[0] & 0x80) == 0) {
		code = (uint8_t)text[0];
	} else if (((uint8_t)text[0] & 0xE0) == 0xC0) {
		code = ((uint32_t)(text[0] & 0x1F) << 6) | ((uint8_t)text[1] & 0x3F);
	} else if (((uint8_t)text[0] & 0xF0) == 0xE0) {
		code = ((uint32_t)(text[0] & 0x0F) << 12) | ((uint32_t)(text[1] & 0x3F) << 6) | ((uint8_t)text[2] & 0x3F);
	} else {
		code = ((uint32_t)(text[0] & 0x07) << 18) | ((uint32_t)(text[1] & 0x3F) << 12) |
			   ((uint32_t)(text[2] & 0x3F) << 6) | ((uint8_t)text[3] & 0x3F);
	}

	if (code < 0x80) {
		if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')) {
			mix->ascii++;
		}
		mix->total++;
		return;
	}
	mix->total++;
	if ((code >= 0x3041 && code <= 0x3096) || (code >= 0x30A1 && code <= 0x30FA) || code == 0x30FC) {
		mix->kana++;
	} else if (code == 0x3005 || code == 0x3006 || (code >= 0x4E00 && code <= 0x9FFF) ||
			   (code >= 0x3400 && code <= 0x4DBF) || (code >= 0xF900 && code <= 0xFAFF)) {
		mix->kanji++;
	} else {
		mix->other++;
	}
}

// Whether what a scan produced is a Japanese text rather than a coincidence of
// bytes. The bar is the same for both encodings: kana or kanji, more of them
// than there are ASCII letters, and no stray character a Japanese tag would
// not carry -- an accented Latin letter coming out of one of these encodings
// means the bytes were never Japanese.
static bool jp_text_is_japanese(const jp_mix_t *mix) {
	int jp = mix->kana + mix->kanji;
	return jp >= 1 && jp >= mix->ascii && mix->other == 0;
}

// Decodes `len` bytes of Shift-JIS into `out` as UTF-8. Returns false -- and
// leaves `out` empty -- when a byte cannot be part of a Shift-JIS text, which
// is how a Latin-1 name is told from a Japanese one.
static bool sjis_to_utf8(const uint8_t *data, size_t len, char *out, size_t out_size, jp_mix_t *mix) {
	size_t oi = 0;
	for (size_t i = 0; i < len;) {
		uint8_t c = data[i];
		const char *text = NULL;
		if (c == 0) {
			break;
		}
		if (c < 0x80) {
			if (oi + 1 >= out_size) {
				return false;
			}
			out[oi++] = (char)c;
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
				mix->ascii++;
			}
			mix->total++;
			i++;
			continue;
		}
		if (c >= 0xA1 && c <= 0xDF) {
			// Halfwidth katakana: one byte, U+FF61 + (c - 0xA1).
			uint32_t code = 0xFF61 + (uint32_t)(c - 0xA1);
			if (oi + 3 >= out_size) {
				return false;
			}
			out[oi++] = (char)(0xE0 | (code >> 12));
			out[oi++] = (char)(0x80 | ((code >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (code & 0x3F));
			mix->kana++;
			mix->total++;
			i++;
			continue;
		}
		uint32_t row, cell;
		if (i + 1 >= len || !sjis_row_cell(c, data[i + 1], &row, &cell)) {
			return false;
		}
		text = jis_char(row, cell);
		if (!text) {
			return false;
		}
		size_t n = strlen(text);
		if (oi + n >= out_size) {
			return false;
		}
		memcpy(out + oi, text, n);
		oi += n;
		jp_count(mix, text);
		i += 2;
	}
	out[oi] = '\0';
	return true;
}

// Decodes `len` bytes of EUC-JP into `out` as UTF-8, with the same contract
// as sjis_to_utf8(). 0x8E opens a halfwidth katakana byte, 0x8F a JIS X 0212
// three-byte character, of which only the first byte's validity is checked
// (JIS X 0212 is not in the table and none of its characters is kana).
static bool euc_to_utf8(const uint8_t *data, size_t len, char *out, size_t out_size, jp_mix_t *mix) {
	size_t oi = 0;
	for (size_t i = 0; i < len;) {
		uint8_t c = data[i];
		if (c == 0) {
			break;
		}
		if (c < 0x80) {
			if (oi + 1 >= out_size) {
				return false;
			}
			out[oi++] = (char)c;
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
				mix->ascii++;
			}
			mix->total++;
			i++;
			continue;
		}
		if (c == 0x8E && i + 1 < len && data[i + 1] >= 0xA1 && data[i + 1] <= 0xDF) {
			uint32_t code = 0xFF61 + (uint32_t)(data[i + 1] - 0xA1);
			if (oi + 3 >= out_size) {
				return false;
			}
			out[oi++] = (char)(0xE0 | (code >> 12));
			out[oi++] = (char)(0x80 | ((code >> 6) & 0x3F));
			out[oi++] = (char)(0x80 | (code & 0x3F));
			mix->kana++;
			mix->total++;
			i += 2;
			continue;
		}
		if (c == 0x8F) {
			// JIS X 0212: three bytes, and no character of the table.
			if (i + 2 >= len || data[i + 1] < 0xA1 || data[i + 1] > 0xFE || data[i + 2] < 0xA1 ||
				data[i + 2] > 0xFE) {
				return false;
			}
			mix->other++;
			mix->total++;
			i += 3;
			continue;
		}
		if (c < 0xA1 || c > 0xFE || i + 1 >= len || data[i + 1] < 0xA1 || data[i + 1] > 0xFE) {
			return false;
		}
		const char *text = jis_char(c - 0x80, data[i + 1] - 0x80);
		if (!text) {
			return false;
		}
		size_t n = strlen(text);
		if (oi + n >= out_size) {
			return false;
		}
		memcpy(out + oi, text, n);
		oi += n;
		jp_count(mix, text);
		i += 2;
	}
	out[oi] = '\0';
	return true;
}

// Whether the bytes hold a lead byte in 0x81..0x9F, which no Latin-1 tag and
// no Windows-1251 tag ever writes.
static bool has_c1_lead(const uint8_t *data, size_t len) {
	for (size_t i = 0; i < len && data[i]; i++) {
		if (data[i] >= 0x81 && data[i] <= 0x9F) {
			return true;
		}
	}
	return false;
}

// Whether `len` bytes are a Japanese tag written without an encoding byte,
// and `out` now holds them in UTF-8. Tried in the order the evidence is
// strongest: Shift-JIS starting with a lead byte in 0x81..0x9F, then any
// Shift-JIS whose text comes out as kana and kanji, then EUC-JP the same way.
// A text that does not decode cleanly, or decodes into something that is not
// mostly kana and kanji, is left to Latin-1 and its accented letters.
bool tagtext_japanese(const uint8_t *data, size_t len, char *out, size_t out_size) {
	if (len < 2 || out_size < 8) {
		return false;
	}
	// A UTF-16 byte order mark, which some taggers leave in a frame that says
	// Latin-1, is not Japanese text either.
	if ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)) {
		return false;
	}

	jp_mix_t sjis = {0};
	bool sjis_ok = sjis_to_utf8(data, len, out, out_size, &sjis);
	if (sjis_ok && has_c1_lead(data, len) && jp_text_is_japanese(&sjis)) {
		return true;
	}

	jp_mix_t euc = {0};
	if (euc_to_utf8(data, len, out, out_size, &euc) && jp_text_is_japanese(&euc)) {
		return true;
	}

	if (sjis_ok && jp_text_is_japanese(&sjis)) {
		return true;
	}
	out[0] = '\0';
	return false;
}


// ---------------------------------------------------------------------------
// The one function the readers call
// ---------------------------------------------------------------------------

// Decodes tag text whose encoding is not reliably known. Text that is already
// UTF-8 is copied as it is -- but only when it holds something above ASCII,
// since a bare ASCII run decodes the same in every code page and Latin-1 is the
// answer the format promises. Then Japanese, then Windows-1251, then Latin-1,
// each on the evidence in the bytes.
bool tagtext_guess(const uint8_t *data, size_t len, char *out, size_t out_size) {
	if (out_size == 0) {
		return false;
	}
	out[0] = '\0';
	if (len == 0) {
		return false;
	}

	if (text_is_wide_utf8(data, len)) {
		size_t copy_len = strnlen((const char *)data, len);
		if (copy_len > out_size - 1) {
			copy_len = out_size - 1;
		}
		memcpy(out, data, copy_len);
		out[copy_len] = '\0';
		return false;
	}
	if (tagtext_japanese(data, len, out, out_size)) {
		return true;
	}
	if (looks_like_cp1251(data, len)) {
		cp1251_to_utf8(data, len, out, out_size);
		return false;
	}

	// Latin-1, one byte at a time: ASCII stands for itself, and everything
	// above 0x7F becomes two bytes of UTF-8. Each character is written only
	// when the room is there for it and for the terminator that follows, so a
	// field that runs out of buffer ends on a whole character, never half of
	// one.
	size_t oi = 0;
	for (size_t i = 0; i < len; i++) {
		uint8_t c = data[i];
		if (c == 0) {
			break;
		}
		size_t need = c < 0x80 ? 1 : 2;
		if (oi + need >= out_size) {
			break;
		}
		if (need == 1) {
			out[oi++] = (char)c;
		} else {
			out[oi++] = (char)(0xC0 | (c >> 6));
			out[oi++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[oi] = '\0';
	return false;
}

// UTF-16 (with or without a byte-order mark) to UTF-8. A byte-order mark is
// consumed rather than written out, and when the caller has not committed to
// an order it decides it: a mark is the one thing in the data that says which
// way round the machine that wrote it was. Callers that already know -- an
// encoding-2 frame is big-endian by definition -- pass the order and only the
// mark itself is skipped.
void tagtext_utf16(const uint8_t *data, size_t len, int big_endian, char *out, size_t out_size) {
	// big_endian: 1 = big, 0 = little, -1 = let the mark decide.
	bool big = big_endian != 0;
	if (len >= 2) {
		if (data[0] == 0xFE && data[1] == 0xFF) {
			big = true;
			data += 2;
			len -= 2;
		} else if (data[0] == 0xFF && data[1] == 0xFE) {
			big = false;
			data += 2;
			len -= 2;
		}
	}
	utf16_to_utf8(data, len, big, out, out_size);
}
