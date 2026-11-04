# Stock firmware input

`v1.zip.part1` and `v1.zip.part2` in this directory are the official TempoTec
Variations V1 **1.3** OTA update: one zip, split into two byte-for-byte pieces
so each fits under GitHub's upload limit. Neither piece opens on its own. Keep
them together, with the same names. The zip holds a single file, `v1.upt`.

To prepare it locally (requires 7-Zip; `prepare_v1_stock.sh` joins the pieces
itself):

```sh
sonix-packer/prepare_v1_stock.sh sonix-packer/firmware-input/v1.zip.part1
sonix-packer/verify_upt.sh sonix-packer/v1_original.upt
```

Joining them by hand gives the same zip:
`cat v1.zip.part1 v1.zip.part2 > v1.zip`.

| File | Size (bytes) | SHA-256 |
| --- | ---: | --- |
| `v1.zip.part1` | 18,191,074 | `cae3f02bcd46aa42487a770e853ed88d4fb3f267a0591603ba6fbccfd6cdbf15` |
| `v1.zip.part2` | 18,191,074 | `9129833f4d77ed3b1169af7348f7e7433c0c36b0e14a028b434563ce83e6735d` |
| `v1.upt` (inside the zip) | 39,925,760 | `ee35ecbf2635463671aa14ffb492880ff8430699987c8c19d0991a3fbdc97530` |

The `v1.upt` MD5 is `91fd8a5f4772c2c4a0e4456a77bda8bc`. The zip carries no
checksum file. If your update procedure wants one beside `v1.upt`,
`md5sum v1.upt > v1_md5.txt` writes the same `<md5>  v1.upt` line the packer
writes for its own image. Its
`usr/resource/config.json` reports product version 1.3, and the verifier
prints:

```
Kernel SHA-256: a598b189a729c8b9c14b4738ad595b0903e4361ef8e596ca63109f603f2f6230
OTA config: current_version=0
Compatibility: tempotec_v1_x1600_auto
TempoTec product version: 1.3
```

What is new in the 1.3 package, compared with the image this directory held
before (`v1_upload.7z.001`/`.002`, labelled 1.3 but carrying product version
1.2 and SHA-256 `9f929a92…fa05d`):

- `ota_v0/ota_compatibility.in` (`compatibility=tempotec_v1_x1600_auto`), a
  file the earlier package does not have. Nothing in the rootfs or the kernel
  reads it. The packer keeps it unchanged, and CI checks the output carries
  the stock file byte-for-byte (`verify_upt.sh --stock`).
- A new kernel (`#120`, built 13 October 2026). It adds support for one more
  SPI NAND chip (Zbit `ZB35Q01CYIG`). The packer never touches the kernel.
- Rebuilt drivers, `sys_server`, `dmrd` (QPlay 3), Bluetooth/USB/Wi-Fi
  scripts. The new script paths target AIC8800 radios and the HiBy R1, so they
  stay inactive on the Broadcom-based V1.

The build workflow uses these pieces by default; its optional stock firmware
URL overrides them. Other official updates can also be supplied to the
prepare script as a raw `v1.upt`, an archive, a directory, or the first piece
of a split archive (`NAME.part1` or `NAME.001`, other pieces beside it).
Extracted UPTs are ignored by Git. Retain the matching official firmware and
checksum for recovery.

The stock 1.3 image contains the launcher and init paths the packer expects.
Packaging checks do not replace testing on a physical V1, and flashing an
unofficial image always carries a risk.
