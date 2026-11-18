# Download music from WebDAV or FTP on the TempoTec V1

**Network folders** is a client: the V1 connects to a server you already run. This is separate from [Wireless → Transfer](wi-fi-transfer.md), where your phone or computer sends files to the V1.

1. Join Wi-Fi on the V1. Insert a writable microSD card to download music; you can browse a server without a card.
2. Open **Streaming → Network folders**, then choose **WebDAV** or **FTP**.
3. Enter the server's full folder URL, such as `https://nas.example.net/dav/Music/` or `ftp://192.168.1.40:2121/Music/`. For Nextcloud, use its WebDAV URL (not the web login page). FTP paths start at the account's login directory. Put credentials in the separate **Username** and **Password** fields, or leave both blank for a public share. The V1 remembers one address and username per protocol; passwords are held in memory until restart.
4. Tap **Connect**. Open an album folder, then tap a track and choose **Save to microSD** or **Save and play**. **Save folder's tracks** downloads the playable files in the current folder, not its subfolders. Stay on the page until the download finishes. **Save and play** starts local playback **after** saving the whole file; it is not live streaming.
5. Find saved files in **Music → Browse**, under `Music/Network Downloads/WebDAV/` or `Music/Network Downloads/FTP/` on the card. Inside that folder the server name and remote folder tree are preserved. Characters FAT cannot store (and `%` itself) are saved as `%XX`. If you use the indexed library, run **Settings → Music → Scan music library** to add the new tracks.

Downloads keep the original file bytes and tags. A file already at the destination is skipped. Cancelling, leaving the page, losing the connection or handing the card to USB stops the current download and discards the unfinished file; completed tracks stay. If power is cut or the card is physically pulled mid-write, you may find a hidden `.sonix-network-*.part` file, which you can delete. The per-file limit is 4 GiB minus one byte, even on exFAT. Large folders are paged on the player; at most 384 entries from a listing are shown, and batch download is disabled when that listing was truncated.

## Server compatibility and credentials

- **WebDAV:** `http://` or `https://`, `PROPFIND` with Depth 1 for browsing and `GET` for downloads. Use the final folder URL; redirects aren't followed. Basic or Digest authentication is supported.
- **FTP:** passive `ftp://` or implicit `ftps://`. The client prefers `MLSD`, then falls back to common Unix and Windows `LIST` formats. SFTP and explicit FTPS (FTP with `AUTH TLS`) aren't supported.
- Use **HTTPS WebDAV** or **FTPS** when available. Plain HTTP and FTP are not encrypted and should only be used on a trusted network. For a private CA, configure its PEM bundle as `[network] ca_bundle` in `device_config.ini`. Network folders checks both the certificate chain and the server name; it doesn't use the relaxed TLS switch for other services.

On a physical V1, check a real server login, a nested album download and playback, then cancel a large download and repeat while switching to USB storage. Verify that completed files open offline and the unfinished track is not shown in **Browse**.
