# Stock firmware input

The split `v1_upload.7z.001` and `v1_upload.7z.002` files in this directory
are the supplied TempoTec V1 1.3 stock update. Keep both parts together. To
prepare them locally (requires 7-Zip):

```sh
sonix-packer/prepare_v1_stock.sh sonix-packer/firmware-input/v1_upload.7z.001
sonix-packer/verify_upt.sh sonix-packer/v1_original.upt
```

The extracted `v1.upt` has SHA-256
`9f929a92319a05a81bbd16cbf0fb8a886a9bb2def192f5a6786b6ea7032fa05d`.
The build workflow uses this archive by default; its optional stock firmware
URL overrides it. Other official updates can also be supplied to the prepare
script as a raw `v1.upt`, an archive, or a directory. Extracted UPTs are
ignored by Git. Retain the matching official firmware and checksum for recovery.

The stock 1.3 image contains the launcher and init paths expected by the
packer. Packaging checks are not a substitute for testing on a physical V1;
flashing an unofficial image always carries a risk.
