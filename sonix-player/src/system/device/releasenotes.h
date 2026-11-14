#ifndef RELEASENOTES_H
#define RELEASENOTES_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/core/json.h"

// GitHub release descriptions are Markdown, but pasted banners and screenshots
// often arrive as HTML. The update card renders Markdown, not a web page: remove
// HTML before bounding the notes, so a long image attribute cannot push the
// actual changelog out of the buffer.
//
// Tags, comments, script/style contents and image attributes disappear; text,
// Markdown and paragraph breaks stay. Common HTML entities become UTF-8. This
// is a display cleanup, not an HTML renderer: nothing is fetched or executed.
//
// `out` holds at most `size - 1` bytes and is always terminated when size > 0.
// A cut UTF-8 character is dropped. Source and destination must not overlap;
// a NULL source produces an empty string.
void release_notes_clean(const char *source, char *out, size_t size);

// Decodes the complete JSON "body" before cleaning it into the bounded output.
// The temporary allocation is bounded by the response size already accepted by
// the caller. False (with empty output) for a missing/non-string body or failed
// allocation; release notes are optional and must not prevent an update.
bool release_notes_load(const json_doc_t *doc, int release, char *out, size_t size);

#endif /* RELEASENOTES_H */
