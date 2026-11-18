# Wi-Fi file transfer on the TempoTec V1

1. Insert a microSD card and connect the V1 and your phone or computer to the same local Wi-Fi network.
2. On the V1, open **Wireless → Transfer** and turn on **Wi-Fi transfer**. If you have the Transfer tile in Control Centre, it can open and start the session too.
3. Scan the QR code with your phone's camera. On a computer, open the **`http://` address shown on the V1** and enter the eight-character code underneath it. If the V1 also shows `sonix-transfer.local`, that address works on clients with mDNS support; use the numeric address if the name doesn't resolve. The browser needs to accept a session cookie.
4. In the browser, open or create the destination folder, such as `Music`. Drop FLAC files or album folders onto the page, or use **Upload files** / **Upload folder**. Folder uploads keep the paths of the files inside them. Each file is saved directly to microSD; wait for the success message before turning off transfer.
5. Turn the switch off or leave the Transfer screen when done. Find the music in **Browse**; use **Settings → Music → Scan music library** if the new tracks do not show up in the indexed library. **Detect changes**, if enabled on an existing library, also checks the card when the session ends.

You can also download files and rename, move, or delete files and folders in the browser. Uploads and moves refuse names that already exist at the destination. For example, if two albums contain `01.flac`, send them in separate album folders. The upload limit is **4 GiB minus one byte per file**, including on exFAT cards. Cancelling or losing the connection discards the unfinished file; tracks that already finished are kept. If power is cut or the card is pulled mid-write, a hidden `.sonix-upload-*.part` file may remain on the card, but it won't appear as a finished track.

**Use a trusted Wi-Fi network only.** This is local HTTP, not HTTPS: the code limits access to the current session but doesn't encrypt it. Don't port-forward the server or share the QR code. A new session needs a new code. The listener is off by default and closes when you leave the screen, turn off Wi-Fi, lose the link, or hand the card to USB storage. It cannot start without a mounted microSD card.

If the page doesn't load, check that the V1 is still on the Transfer screen and use the exact address shown, including the port if there is one. Guest networks sometimes block devices from reaching one another. This is the browser upload option; it doesn't run an FTP/WebDAV server or advertise to the HiBy app. To connect in the other direction, see [Network folders](network-folders.md).

## Device check after flashing

- Scan the QR code with a phone camera, then try the typed address/code in a different browser on a laptop. Confirm both open the same card and that a browser without the code cannot list it.
- Drop an album with a nested folder, Unicode titles and artwork. Verify the saved layout and play a newly uploaded track. Try a duplicate and confirm the original is unchanged.
- Cancel a large upload, then repeat while switching transfer off, disconnecting Wi-Fi, or exporting the card over USB. Verify the address stops responding, the incomplete file isn't present, and the next session rejects the old code.
