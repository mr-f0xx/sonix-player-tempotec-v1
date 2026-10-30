# Local stock firmware input

Place the official TempoTec V1 `v1.upt` file for the firmware version you want to inspect in this folder. This is a local input area; the firmware image is intentionally excluded from Git and must not be committed or redistributed.

After placing the file, tell the coding agent it is available at `sonix-packer/firmware-input/v1.upt`. The packer workflow can then prepare a private working copy and inspect/build from it.
