# EXT2 source consolidation — 6 November 2026

The independent `Filesystem-EXT2` repository owns the portable EXT2 core, shared-object adapter, Linux VFS driver sources, EFI reader sources, and Windows driver adapter sources.

## Reconciliation

The portable core, V1 `.so` adapter and design files already present in this repository were byte-identical to `Filesystems` main at `a253fb724ce1fd760a361219281effb6abd71367`. These were preserved rather than replaced with duplicate or older copies.

The remaining `native/filesystems/ext2/{linux,efi,windows}/` source files were copied from that same central main commit, with their paths and notices preserved.

## Local build ownership

`standalone/kernel.json` now names the in-repository `native/filesystems/ext2` tree without a remote `revision`. `standalone/kernel.py` therefore builds `ext2.ko` using local source packaged in this repository. The embedded native installer no longer downloads a historical central Filesystems archive for EXT2 kernel source.

`EXT2.so` continues to be compiled from this repository's own portable core. The kernel module is a separate binary built from the same authoritative format code with its Linux VFS adapter.

## What this does *not* prove

Compilation alone does not qualify read/write data integrity, root boot, or EFI/Windows deployment. Existing test gates and kernel ABI checks remain mandatory. The central Filesystems copy must not be removed from the manager build until every active consumer/build/CI reference has migrated to this repository, and the post-migration build is green. That deletion is a separate explicit change; do not maintain two independent format engines thereafter.

Source provenance: `Infiltrator-Projects/Filesystems@a253fb724ce1fd760a361219281effb6abd71367`, originally copied from and modified with the upstream Linux implementation subject to its retained license.
