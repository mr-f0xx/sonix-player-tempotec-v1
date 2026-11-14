#ifndef TAGTEXT_H
#define TAGTEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// The characters in a tag written without an encoding
//
// An ID3v2 text frame says what encoding it uses in a byte of its own, and
// that byte is 0x00 more often than not: Latin-1, as the spec has it. But the
// tagger that wrote it wrote the machine's own code page instead, and left the
// byte alone. On a Japanese Windows that is Shift-JIS, in an older Unix tool
// EUC-JP, on a Russian machine Windows-1251, and on a Western one either
// Latin-1 proper or -- since taggers started guessing -- UTF-8.
//
// One function settles all of it, so that every reader of tag text decodes the
// same bytes the same way: the ID3v2 frames and the ID3v1 fields in
// metadata.c, the chapter titles in id3chap.c, and whatever else reads a tag
// later.
// ---------------------------------------------------------------------------

// Decodes tag text of `len` bytes, whose encoding is not reliably known, into
// UTF-8 in `out`, which is always NUL-terminated. Text that is already UTF-8
// is copied as it is; otherwise Japanese, then Windows-1251, then Latin-1 are
// tried, in that order, on the evidence in the bytes themselves. Returns true
// when the bytes were Japanese and came out as kana and kanji.
bool tagtext_guess(const uint8_t *data, size_t len, char *out, size_t out_size);

// The same for Shift-JIS and EUC-JP read as one character set, JIS X 0208: no
// guessing, these two only, false when the bytes are not cleanly one of them
// or do not come out mostly kana and kanji. tagtext_guess() is this plus the
// Latin-1 and Windows-1251 cases.
bool tagtext_japanese(const uint8_t *data, size_t len, char *out, size_t out_size);

// UTF-16 (with or without a byte-order mark) to UTF-8, stopping at a null
// terminator. A byte-order mark is consumed, not written, and it overrides
// `big_endian` -- the mark is the only thing in the data that says which way
// round the machine that wrote it was. Characters above U+FFFF are written as
// '?'.
void tagtext_utf16(const uint8_t *data, size_t len, int big_endian, char *out, size_t out_size);


// The length of the UTF-8 sequence opened by `c` (0 when `c` cannot open one),
// and whether `len` bytes are well-formed UTF-8 up to the first NUL. Shared with the tag readers that check text on their own.
size_t tagtext_utf8_length(uint8_t c);
bool tagtext_is_utf8(const uint8_t *data, size_t len);

#endif // TAGTEXT_H
