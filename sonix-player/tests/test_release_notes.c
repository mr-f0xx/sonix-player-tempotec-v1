// Wi-Fi update notes: GitHub's Markdown/HTML description becomes readable
// Markdown before the device's small notes buffer is applied. No network or
// LVGL is needed; the JSON parser and production cleanup are the real ones.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/device/ota.h"
#include "src/system/device/releasenotes.h"

static void clean_is(const char *source, const char *want) {
	char out[OTA_NOTES_MAX];
	release_notes_clean(source, out, sizeof(out));
	if (strcmp(out, want) != 0) {
		fprintf(stderr, "release notes:\n  got:  %s\n  want: %s\n", out, want);
		assert(!"unexpected release notes");
	}
}

static void test_github_banner(void) {
	// The opening tag from the reported 1.6.2.1 release. Its alt text is not
	// changelog text, and neither are its dimensions or attachment URL.
	clean_is("<img width=\"1664\" height=\"624\" alt=\"D8As1YMQxmTvAAAAJXRFWHRkYXRlOmNyZWF0ZQAyMDI2LTExLTEzVDE5OjI5OjE\" "
			 "src=\"https://github.com/user-attachments/assets/cdaf32ef-81f0-4c23-8d11-962c5a540be2\" />\r\n\r\n"
			 "## What's Changed\r\n\r\n* Calculator: accent frame\r\n* Music: search button\r\n\r\n"
			 "**Full Changelog**: https://github.com/mr-f0xx/sonix-player-tempotec-v1/compare/1.6.2...1.6.2.1",
			 "## What's Changed\n\n* Calculator: accent frame\n* Music: search button\n\n"
			 "**Full Changelog**: https://github.com/mr-f0xx/sonix-player-tempotec-v1/compare/1.6.2...1.6.2.1");
	clean_is("<IMG ALT='a > b' SRC='https://example.invalid/banner.png'>\n## Fixes\n- One", "## Fixes\n- One");
	clean_is("<img\n width=\"1664\"\n alt=\"quoted > and ' text\"\n src=\"banner.png\"\n/>\n- After", "- After");
	clean_is("<picture><source srcset=\"banner.webp\"><img src=\"banner.png\" alt=\"banner\"></picture>\n"
			 "<p>Readable <strong>changelog</strong>.</p><p>Next paragraph.</p>",
			 "Readable changelog.\n\nNext paragraph.");
	clean_is("<img src=\"one.png\">\n\n\n\n<img src=\"two.png\">\n\n- Only text", "- Only text");
	clean_is("<img src=\"banner.png\" alt=\"only a banner\" />", "");
	clean_is("<img src=\"unfinished", "");
}

static void test_markdown_and_text(void) {
	clean_is("## Changes\r\n- **Bold** and `code`\r\n  - Nested\r\n\r\n[Link](https://example.invalid) and ![image](image.png)",
			 "## Changes\n- **Bold** and `code`\n  - Nested\n\n[Link](https://example.invalid) and ![image](image.png)");
	clean_is("Use <https://example.invalid> when a < b and 2 < 3.",
			 "Use <https://example.invalid> when a < b and 2 < 3.");
	clean_is("<!-- hidden\nover lines -->## Visible\nBefore<!-- inline --> after\n<!-- unfinished",
			 "## Visible\nBefore after");
	clean_is("<style>body { color: red; }</style><script>if (a > b) run();</script>## Notes\n- Safe",
			 "## Notes\n- Safe");
	clean_is("<p>Wi-Fi &amp; Bluetooth &quot;fixed&quot;.</p><p>25&#xB0;C &#x1F3B5; &lt;stdio.h&gt;</p>",
			 "Wi-Fi & Bluetooth \"fixed\".\n\n25°C 🎵 <stdio.h>");
	clean_is("Line one<br/>Line two<hr><div>Block</div>", "Line one\nLine two\n\nBlock");
	clean_is("&amp;lt; stays escaped; &#0; &#xD800; &#x110000; &unknown; stay literal",
			 "&lt; stays escaped; &#0; &#xD800; &#x110000; &unknown; stay literal");
}

static void test_bounds(void) {
	char out[32];
	memset(out, 'X', sizeof(out));
	release_notes_clean("123456789", out, 4);
	assert(strcmp(out, "123") == 0 && out[4] == 'X');
	memset(out, 'X', sizeof(out));
	release_notes_clean("abc", out, 1);
	assert(out[0] == '\0' && out[1] == 'X');
	release_notes_clean(NULL, out, sizeof(out));
	assert(out[0] == '\0');
	release_notes_clean("abc", NULL, 0);

	// Every cut through a multi-byte character must leave valid UTF-8, including
	// cuts after only the leading byte of a four-byte emoji.
	const char *text = "aé🎵z";
	for (size_t size = 1; size <= 12; size++) {
		char small[13];
		memset(small, 'X', sizeof(small));
		release_notes_clean(text, small, size);
		assert(strlen(small) < size);
		assert(small[size] == 'X');
		const unsigned char *p = (const unsigned char *)small;
		while (*p) {
			size_t need = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
			for (size_t i = 1; i < need; i++) {
				assert((p[i] & 0xC0) == 0x80);
			}
			p += need;
		}
	}
}

// Escapes a release body into a GitHub-shaped JSON response. In particular,
// image attributes longer than OTA_NOTES_MAX must not consume the output cap.
static void check_json_body(const char *body, const char *want) {
	char *json = malloc(strlen(body) * 6 + 256);
	assert(json);
	size_t n = 0;
	json[n++] = '{';
	json[n++] = '"';
	memcpy(json + n, "body", 4);
	n += 4;
	json[n++] = '"';
	json[n++] = ':';
	json[n++] = '"';
	for (const char *p = body; *p; p++) {
		switch (*p) {
		case '"': json[n++] = '\\'; json[n++] = '"'; break;
		case '\\': json[n++] = '\\'; json[n++] = '\\'; break;
		case '\n': json[n++] = '\\'; json[n++] = 'n'; break;
		case '\r': json[n++] = '\\'; json[n++] = 'r'; break;
		default: json[n++] = *p; break;
		}
	}
	json[n++] = '"';
	json[n++] = '}';
	json[n] = '\0';
	json_doc_t doc;
	assert(json_parse(json, &doc));
	char notes[OTA_NOTES_MAX];
	assert(release_notes_load(&doc, json_root(&doc), notes, sizeof(notes)));
	if (strcmp(notes, want) != 0) {
		fprintf(stderr, "JSON release notes:\n  got:  %s\n  want: %s\n", notes, want);
		assert(!"unexpected JSON release notes");
	}
	json_free(&doc);
	free(json);
}

static void test_json_before_cap(void) {
	const char *tail = "\n## What's Changed\n- The changelog survives the banner.";
	char *body = malloc(OTA_NOTES_MAX * 3);
	assert(body);
	int header = snprintf(body, OTA_NOTES_MAX * 3, "<img alt=\"");
	memset(body + header, 'A', OTA_NOTES_MAX * 2);
	strcpy(body + header + OTA_NOTES_MAX * 2, "\" src=\"https://example.invalid/banner.png\" />");
	strcat(body, tail);
	check_json_body(body, tail + 1);
	free(body);

	check_json_body("<img src=\"banner.png\" />\n## Unicode \xC3\xA9\xF0\x9F\x8E\xB5",
					"## Unicode é🎵");
	char out[OTA_NOTES_MAX] = "sentinel";
	json_doc_t doc;
	assert(json_parse("{}", &doc));
	assert(!release_notes_load(&doc, json_root(&doc), out, sizeof(out)) && out[0] == '\0');
	json_free(&doc);
	assert(json_parse("{\"body\":null}", &doc));
	assert(!release_notes_load(&doc, json_root(&doc), out, sizeof(out)) && out[0] == '\0');
	json_free(&doc);
	assert(!release_notes_load(NULL, -1, out, sizeof(out)) && out[0] == '\0');
}

int main(void) {
	test_github_banner();
	test_markdown_and_text();
	test_bounds();
	test_json_before_cap();
	puts("PASS release notes: HTML banners removed, Markdown kept, JSON decoded before the size cap, UTF-8 bounds");
	return 0;
}
