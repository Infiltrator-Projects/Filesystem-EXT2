# EXT2 independent repository migration (2026-10-30)

The canonical freestanding EXT2 core, legacy V1 module and independent synthetic-volume regression test were copied **without modification** from Infiltrator-Projects/Filesystems commit ca2fa1210c3c1f2b6f52cd15b3da6dfa83d453ad.

`make` builds `EXT2.so` from the canonical source, and `make test` runs the existing module regression. This is **not** full create/delete/defrag/root/EFI qualification: current V1 writes only previously allocated bytes of existing files. Linux/EFI/Windows host adapters remain in the original Filesystems tree until their build integration is independently migrated. Defragmenter's EXT2/EXT3/EXT4 shared relocation engine remains in Defragmenter during transition; do not fork divergent copies into all three repositories.

Existing original paths remain untouched, no consumer switched, and the new Common filesystem ABI is not yet implemented in EXT2.
