# Tag Manager

Open **More → Tag Manager**, choose a folder on the MicroSD card, then tap a
FLAC or MP3. The page shows its filename, artwork (embedded first, then a
folder cover), title, artist, album, album artist, date, track number and genre.
Tap **Track number** or **Genre**, edit using the on-screen keyboard, then tap
**Save**. Use the back arrow to cancel an edit, return to the folder, or move
up a folder. The list is scrollable and shows at most 200 entries per folder.

Track numbers accept a positive number or `number/total` (for example `3/12`).
The editor initially shows the track number only; enter `/total` again if you
want to retain it. Empty values and tag deletion are not supported. Rescan the
music library after editing if its cached entries do not yet show the changes.

## Safety and compatibility

Back up your card before editing. Stop playback of the file being edited. Each
save copies the entire file to a temporary file on the same card, flushes it,
then replaces the original; allow at least the file's size in free space and do
not remove the card or turn off the player during the operation. Large files
can take a while and the interface waits during the copy. A power failure may
leave a `.sonix-...` temporary file alongside the original.

The writer supports native FLAC Vorbis comments and ordinary ID3v2.3/v2.4 MP3
tags. MP3 files without an ID3v2 tag, v2.2, unsynchronised/extended/footer
headers, multiple consecutive ID3 tags, and flagged target frames are refused
rather than rewritten. ID3v2.3 edits currently accept ASCII only; ID3v2.4 and
FLAC accept UTF-8. Complex, malformed, or over-16 MiB metadata is refused.
The writer retains other metadata and artwork; it retains existing ID3 padding
when possible and refuses to move audio past known offset-bearing chapter/seek
frames. Symlinks, hard links, read-only files and files owned by another user
are not edited. File permissions are retained, but filesystem-specific extended
attributes are not copied. Editing replaces the file inode and may change its
modification time. The app is intended for files on the card, not for files
being shared over USB or edited concurrently elsewhere.
