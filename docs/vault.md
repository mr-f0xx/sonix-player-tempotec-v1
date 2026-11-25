# Pocket vault (Keybox)

An optional More tool for the passwords and recovery codes that should not sit
in the open on a microSD card: one row per title, the password underneath it,
up to 200 rows. It is modelled on Rockbox's **Keybox** and
is deliberately the opposite of a password manager account -- there is no
account, no sync, no network and no phone. The card is the only copy.

> **There is no recovery.** The PIN is not stored and no recovery key exists. A
> forgotten PIN means every entry is gone. So does losing the card with an
> attacker willing to work on the file. Read [What this does and does not
> protect](#what-this-does-and-does-not-protect) before trusting it with
> something you cannot lose.

## Using it

**More → Vault** asks for a PIN the first time. Type 4 to 12 digits twice; the
page says when the two do not match. A new vault opens on an empty list with
one accent row, **New password**. Tap it and type a **Title** (Wi-Fi, bank,
email) and the **Password** below it; the check in the corner, or on the
keyboard, saves the row and returns to the list. Tapping a row reopens it for
editing, and the eye beside the password shows or hides it. The trash in the
corner deletes the entry that is open, after asking first. The ellipsis opens
three choices for the whole vault:

- **Change PIN** -- re-encrypts every entry under a new PIN with a new salt.
  The old PIN stops working as soon as it finishes.
- **Lock now** -- puts the keys away without leaving the page.
- **Erase vault** -- asks first. Erasing removes the file; there is no undo and
  no copy.

The vault locks itself after **two minutes without typing**, and saves the
entry being edited first if the card will take it. Leaving the page also saves
and locks. The page never draws the PIN, only a row of dots, and the editor
keeps no cut/copy history.

Up to **200 entries** fit in one vault, and all of them together may take
**64 KB** (65,536 bytes) before encryption. A title is one line of up to 96
bytes; a password may be up to 16 KB, so a long seed phrase or a note can still
live in one row. Both fields are UTF-8. The list shows the first line of each
password, dimmed under its title, with an eye to mask it.

## What is on the card

One file: `Vault/vault.kbx` in the root of the microSD card. Nothing else is
needed, and nothing is written to the player's own settings, so the card can be
moved to another V1 and opened with the same PIN.

The file is binary, mode `0600`, and carries no plaintext and no PIN. It is
written to `vault.kbx.tmp` in the same folder, flushed and synced, then renamed
over the old file, so a card pulled out mid-save leaves the previous vault
rather than half of a new one.

## The format, and the cryptography

```text
offset  size  field
0       8     "SONIXVLT"
8       2     version, big-endian (2)
10      2     reserved, zero
12      4     PBKDF2 iteration count, big-endian
16      16    salt
32      16    AES counter
48      4     ciphertext length, big-endian
52      n     ciphertext
52+n    32    HMAC-SHA256 tag
```

The plaintext is one length-prefixed table, so a save writes every entry
together rather than leaving a file per password:

```text
"VLT2"
entry count, big-endian (0..200)
for each entry:
    title length, big-endian
    secret length, big-endian
    title bytes
    secret bytes
```

The keys come from the PIN alone:

```text
keys    = PBKDF2-HMAC-SHA256(PIN, salt, iterations, 64)
enc_key = keys[0..31]
mac_key = keys[32..63]
```

The table is encrypted with **AES-256 in counter (CTR) mode** under `enc_key`
and the random 128-bit counter. The tag is `HMAC-SHA256(mac_key, header ||
ciphertext)`, computed over every byte before it. On opening, the tag is checked
**before** anything is decrypted; a wrong PIN, a flipped byte and a card that
was pulled out are then the same answer -- the vault does not open. A count or
length field that does not add up is refused before anything is allocated for
it. The header
is inside the tag, so the iteration count, salt and counter cannot be changed
underneath the ciphertext.

The defaults are **120,000 PBKDF2 iterations**, a new random salt on creation
and a fresh random counter on every save. A file may carry 1,000 to 2,000,000
iterations; the count is read from the file so an older vault keeps opening
after a future change to the default. The salt and counter come from
`/dev/urandom`; if it cannot be read, nothing is written.

The implementation is plain C with no LVGL and no OS crypto library:

- `src/system/crypto/aes256.c`
- `src/system/crypto/kdf.c` (HMAC-SHA256 and PBKDF2)
- `src/system/core/sha256.c`
- `src/system/vault/vaultstore.c`

It is checked against published vectors in `tests/test_vault.c`: the AES-256
block vectors from FIPS 197, the counter-mode vector from NIST SP 800-38A, and
the HMAC-SHA256 and PBKDF2 answers from RFC 4231 and RFC 7914's published
PBKDF2-SHA256 tests. The same test creates a vault on a temporary card and takes
it through adding, editing and removing entries, the 200-entry and 64 KB
ceilings, a wrong PIN, a flipped ciphertext byte, an edited iteration count, a
truncated file, a PIN change, erasure, and a version-1 file written by hand so
the migration to the table is tested against the format that shipped.

```sh
cd sonix-player/tests && make vault
make -C sonix-player/tests tools-ui   # the page on the real renderer
```

## What this does and does not protect

It protects the entries **at rest on the card**: someone who finds the card,
reads it in a computer, or copies the file cannot read a title or a password
without the PIN, and cannot tell whether a guess was close. It also protects
against a casual look at the player, because nothing is in the file in the open
and the page locks itself.

It does **not** protect against:

- **Guessing the PIN.** A 4-digit PIN is 10,000 possibilities and 120,000
  PBKDF2 iterations is a delay, not a wall. On a desktop GPU an attacker can try
  a short PIN quickly; a 6- to 8-digit PIN is a great deal better. Choose the
  longest PIN you are willing to type on the glass keypad, and never reuse the
  PIN of a bank card or a door.
- **A player that is already unlocked.** While the vault is open its entries
  and keys are in RAM. The page locks after two minutes, on leaving, and on a
  restart, but software running on the device -- or someone holding it while it
  is open -- can read what is on the screen. The list shows passwords unless the
  eye on each row is used; the editor masks the password until its eye is
  tapped.
- **Remnants in memory.** The session's entries and keys are overwritten before
  they are freed, and the editor's fields are cleared when the list returns.
  LVGL's own freed textarea buffers are not scrubbed; that is a limitation of
  the toolkit, not a promise this page can make. Locking the page and
  restarting the player is the clean answer.
- **A compromised firmware.** If the build itself is not trusted, it can log
  the PIN as it is typed. The vault is a feature of the firmware, not a separate
  secure element: the V1 has none.
- **Backups and copies.** Every copy of `vault.kbx` is another copy to attack,
  and erasing the vault removes the one on this card only. If you copy the card,
  copy the whole `Vault` folder deliberately -- a half-copied `.kbx` is simply a
  corrupt vault -- and treat the backup like the secret itself.

There is no network code in this feature, no telemetry and no cloud. The
optional module can be switched off on **Settings → Plugins**, which also keeps
its pages from being built at start-up.

## Recovering from a damaged file

Usually there is nothing to recover: the tag covers the header and the
ciphertext, so a file that fails to open is not a file that can be repaired by
hand. A vault that does not open after a card error can sometimes be restored
from a backup of the `Vault` folder. Re-creating it means erasing it and typing
the entries again; the PIN does not help with a damaged ciphertext.

If the player says **Cannot open the vault** for a file that was working, check
the card first -- `Settings → System → SD card health` is read-only -- and copy
the file off before writing anything else to the card.

## Where it lives in the source

| Path | What it is |
| --- | --- |
| `src/system/vault/vaultstore.{h,c}` | The file format, the entry table, PBKDF2 keys, sealing, opening, erasing. No UI. |
| `src/gui/vault/vaultpage.{h,c}` | The three faces of the page: PIN pad, entry list and editor, the idle lock and the menu. |
| `src/system/crypto/aes256.{h,c}`, `kdf.{h,c}` | AES-256/CTR, HMAC-SHA256 and PBKDF2. |
| `tests/test_vault.c` | Known-answer tests and the store's failure cases on a temporary card. |
| `tests/ui/test_tools_pages.c` | The page on the renderer: creating, adding and editing rows, locking, changing the PIN and erasing. |

A version-1 file (one encrypted text block) still opens: its text becomes one
entry titled **Vault**, and the next save writes the version-2 table. That is
the only migration path, and it needs no extra pass over the card.

The module is registered in `src/system/addons/addons.c` as `vault`; its tile is
gate-checked in `morepage.c` and `gridpage.c`, and `gui.c` builds the page only
when the module is on.
