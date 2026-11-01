# Tag Manager

Open **More → Tag Manager** to browse the card's **Music** folder and its
subfolders. If Music does not exist, create it on the MicroSD card; the manager
does not fall back to the card root. Select a FLAC or MP3 to see its tags and
cover (embedded first, then a folder image). Tap **Edit title**, **Edit artist**,
**Edit track number**, or **Edit genre**, type the new value, and tap **OK**.
The back arrow cancels an edit or goes back up; it cannot go above Music. The
scrollable list shows at most 200 entries per folder.

Track numbers accept a positive number or `number/total` (for example `3/12`).
The editor initially shows the track number only; enter `/total` again if you
want to retain it. Empty values and tag deletion are not supported. Rescan the
music library after editing if its cached entries do not yet show the changes.
Title, artist and track-number edits are refused when a `.tags` sidecar exists:
that file overrides the embedded values for cached streaming tracks. Genre and
cover edits remain available.

## Find cover art over Wi-Fi

On a file's tag page, tap **Find cover art**. The search starts with the tagged
artist and album; change it if necessary and press **OK** to search Apple's
online music catalog. This sends the search text to Apple and requires Wi-Fi,
working HTTPS certificates, a correct device clock and the catalog service.
Select an album to download and preview its image, then choose **Use this cover**
to replace the **selected file's** embedded front cover. Other tracks in the
album are not changed. Back out of the preview to cancel. Search results are
limited to eight, and JPEG downloads to 2 MiB. Searches and downloads run in
the background; service availability, regional results and image rights depend
on the provider.

## Safety and compatibility

Back up your card before editing. Stop playback of the file being edited. Each
save copies the entire file to a temporary file on the same card, flushes it,
then replaces the original; allow at least the file's size in free space and do
not remove the card or turn off the player during the operation. Large files
can take a while. A power failure may leave a `.sonix-...` temporary file
alongside the original.

The writer supports native FLAC Vorbis comments and ordinary ID3v2.3/v2.4 MP3
tags. MP3 files without an ID3v2 tag, v2.2, unsynchronised/extended/footer
headers, multiple consecutive ID3 tags, and flagged target frames are refused
rather than rewritten. UTF-8 values are written as UTF-16 where ID3v2.3 needs
it. Complex, malformed, or over-16 MiB metadata is refused. Other metadata,
non-front pictures and audio are retained. Existing ID3 padding is retained
when possible; moving audio past known offset-bearing chapter/seek frames is
refused. Symlinks, hard links, read-only files and files owned by another user
are not edited. File permissions are retained, but filesystem-specific extended
attributes are not copied. Editing replaces the file inode and may change its
modification time. Do not edit files being shared over USB or edited elsewhere.
