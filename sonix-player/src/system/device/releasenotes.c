#include "releasenotes.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Only presentation is removed here. Markdown is deliberately left for the
// update card's heading, bullet, bold and Markdown-image renderer.
typedef struct {
	char *data;
	size_t size;
	size_t used;
} notes_out_t;

static void append(notes_out_t *out, const char *text, size_t len) {
	size_t room = out->size - 1 - out->used;
	size_t n = len < room ? len : room;
	memcpy(out->data + out->used, text, n);
	out->used += n;
}

// At most one blank line between paragraphs. A removed banner must not leave a
// screenful of empty lines, or spend the notes allowance on those empty lines.
static void newline(notes_out_t *out, int count) {
	if (out->used == 0) {
		return;
	}
	while (out->used > 0 && (out->data[out->used - 1] == ' ' || out->data[out->used - 1] == '\t')) {
		out->used--;
	}
	if (out->used == 0) {
		return;
	}
	size_t have = 0;
	while (have < out->used && have < 2 && out->data[out->used - 1 - have] == '\n') {
		have++;
	}
	for (int i = 0; i < count && have < 2; i++) {
		append(out, "\n", 1);
		have++;
	}
}

static bool name_char(char c) { return isalnum((unsigned char)c) || c == '-'; }

static bool tag_name_is(const char *name, const char *const *names) {
	for (size_t i = 0; names[i]; i++) {
		if (strcasecmp(name, names[i]) == 0) {
			return true;
		}
	}
	return false;
}

// Finds a tag's real '>', not a '>' inside a quoted alt text, URL or attribute.
// Tags may span lines. An unfinished tag hides the remainder rather than leaking
// its attributes into the changelog.
static const char *tag_end(const char *p) {
	char quote = 0;
	for (; *p; p++) {
		if (quote) {
			if (*p == quote) {
				quote = 0;
			}
		} else if (*p == '"' || *p == '\'') {
			quote = *p;
		} else if (*p == '>') {
			return p + 1;
		}
	}
	return p;
}

static bool raw_text_tag(const char *name) {
	static const char *const names[] = {"script", "style", NULL};
	return tag_name_is(name, names);
}

static bool block_tag(const char *name) {
	static const char *const names[] = {"p", "div", "section", "article", "header", "footer", "h1", "h2", "h3", "h4",
										"h5", "h6", "ul", "ol", "li", "blockquote", "pre", "table", "tr", "td", "th",
										"dl", "dt", "dd", "figure", "figcaption", "details", "summary", "hr", "br", NULL};
	return tag_name_is(name, names);
}

// The contents of script/style are not release-note text. Skip through the
// matching closing tag, including any '>' characters inside the script itself.
static const char *raw_text_end(const char *p, const char *name) {
	size_t len = strlen(name);
	for (; *p; p++) {
		if (*p == '<' && p[1] == '/' && strncasecmp(p + 2, name, len) == 0 &&
			(p[2 + len] == '>' || isspace((unsigned char)p[2 + len]))) {
			return tag_end(p);
		}
	}
	return p;
}

static void append_utf8(notes_out_t *out, unsigned int cp) {
	char bytes[4];
	size_t len;
	if (cp < 0x80) {
		bytes[0] = (char)cp;
		len = 1;
	} else if (cp < 0x800) {
		bytes[0] = (char)(0xC0 | (cp >> 6));
		bytes[1] = (char)(0x80 | (cp & 0x3F));
		len = 2;
	} else if (cp < 0x10000) {
		bytes[0] = (char)(0xE0 | (cp >> 12));
		bytes[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[2] = (char)(0x80 | (cp & 0x3F));
		len = 3;
	} else {
		bytes[0] = (char)(0xF0 | (cp >> 18));
		bytes[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		bytes[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		bytes[3] = (char)(0x80 | (cp & 0x3F));
		len = 4;
	}
	append(out, bytes, len);
}

// A small, single-pass entity decoder. Decoded '<' is output, never rescanned as
// a tag, so escaped examples such as &lt;stdio.h&gt; remain readable text.
static const char *entity(const char *p, notes_out_t *out) {
	static const struct {
		const char *name;
		const char *value;
	} named[] = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
				 {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"}, {"bull", "\xE2\x80\xA2"},
				 {"hellip", "\xE2\x80\xA6"}, {"copy", "\xC2\xA9"}, {"deg", "\xC2\xB0"}, {NULL, NULL}};
	const char *semi = strchr(p + 1, ';');
	if (!semi || semi - p > 32) {
		return NULL;
	}
	size_t len = (size_t)(semi - p - 1);
	for (size_t i = 0; named[i].name; i++) {
		if (strlen(named[i].name) == len && strncmp(p + 1, named[i].name, len) == 0) {
			append(out, named[i].value, strlen(named[i].value));
			return semi + 1;
		}
	}
	if (len < 2 || p[1] != '#') {
		return NULL;
	}
	bool hex = p[2] == 'x' || p[2] == 'X';
	size_t start = hex ? 3 : 2;
	if (start >= len + 1) {
		return NULL;
	}
	unsigned int cp = 0;
	for (size_t i = start; i <= len; i++) {
		unsigned int digit;
		char c = p[i];
		if (c >= '0' && c <= '9') {
			digit = (unsigned int)(c - '0');
		} else if (hex && c >= 'a' && c <= 'f') {
			digit = (unsigned int)(c - 'a' + 10);
		} else if (hex && c >= 'A' && c <= 'F') {
			digit = (unsigned int)(c - 'A' + 10);
		} else {
			return NULL;
		}
		if (cp > (0x10FFFF - digit) / (hex ? 16U : 10U)) {
			return NULL;
		}
		cp = cp * (hex ? 16U : 10U) + digit;
	}
	if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
		return NULL;
	}
	append_utf8(out, cp);
	return semi + 1;
}

// Drops a cut character, not a complete UTF-8 sequence preceding it.
static void trim_partial_utf8(notes_out_t *out) {
	size_t start = out->used;
	while (start > 0 && out->used - start < 3 && ((unsigned char)out->data[start - 1] & 0xC0) == 0x80) {
		start--;
	}
	if (start == 0) {
		return;
	}
	unsigned char lead = (unsigned char)out->data[start - 1];
	size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
	if (out->used - (start - 1) < need) {
		out->used = start - 1;
	}
}

void release_notes_clean(const char *source, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	if (!source) {
		return;
	}
	notes_out_t w = {out, size, 0};
	const char *p = source;
	while (*p && w.used + 1 < size) {
		if (*p == '<') {
			if (strncmp(p, "<!--", 4) == 0) {
				const char *close = strstr(p + 4, "-->");
				p = close ? close + 3 : p + strlen(p);
				continue;
			}
			if (p[1] == '!' || p[1] == '?') {
				p = tag_end(p + 2);
				continue;
			}
			bool closing = p[1] == '/';
			const char *name_start = p + 1 + closing;
			if (isalpha((unsigned char)*name_start)) {
				const char *name_end = name_start + 1;
				while (name_char(*name_end)) {
					name_end++;
				}
				// Leave comparisons and Markdown autolinks (<https://...>) alone.
				if (*name_end == '>' || *name_end == '/' || isspace((unsigned char)*name_end)) {
					char name[32];
					size_t len = (size_t)(name_end - name_start);
					if (len < sizeof(name)) {
						memcpy(name, name_start, len);
						name[len] = '\0';
					} else {
						name[0] = '\0';
					}
					p = tag_end(name_end);
					if (!closing && raw_text_tag(name)) {
						p = raw_text_end(p, name);
					} else if (block_tag(name)) {
						bool single = strcasecmp(name, "hr") == 0 || strcasecmp(name, "br") == 0;
						newline(&w, single ? 1 : 2);
					}
					continue;
				}
			}
		}
		if (*p == '&') {
			const char *next = entity(p, &w);
			if (next) {
				p = next;
				continue;
			}
		}
		if (*p == '\r' || *p == '\n') {
			newline(&w, 1);
			p += *p == '\r' && p[1] == '\n' ? 2 : 1;
			continue;
		}
		append(&w, p, 1);
		p++;
	}
	while (w.used > 0 && isspace((unsigned char)out[w.used - 1])) {
		w.used--;
	}
	trim_partial_utf8(&w);
	out[w.used] = '\0';
}

bool release_notes_load(const json_doc_t *doc, int release, char *out, size_t size) {
	if (!out || size == 0) {
		return false;
	}
	out[0] = '\0';
	int body = json_get(doc, release, "body");
	if (body < 0 || body >= doc->count || doc->toks[body].type != JSON_STRING) {
		return false;
	}
	// JSON escapes only shrink the decoded string. The HTTP response limit also
	// bounds this allocation; do not use a second fixed-size stack buffer.
	size_t capacity = (size_t)(doc->toks[body].end - doc->toks[body].start) + 1;
	char *raw = malloc(capacity);
	if (!raw) {
		return false;
	}
	bool ok = json_str(doc, body, raw, capacity);
	if (ok) {
		release_notes_clean(raw, out, size);
	}
	free(raw);
	return ok;
}
