# Filesystem-EXT2

## 1. Purpose and format scope

Second extended filesystem, without a native journal. This repository owns the format-specific implementation of `EXT2.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

Superblock and group descriptors locate inode and block allocation bitmaps and inode tables. File data may use direct and indirect block references; validate range and allocation ownership before mutation.

## 3. Common V2 implementation and capabilities

Hosted Common V2 declares probe, mount, stat, read, write, CREATE and format. The latest ABI qualification explicitly requires CREATE. Native formatting and allocation are guarded by host access. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

A hosted CREATE callback is not evidence of arbitrary filesystem repair or crash-atomic writes. EXT2 has no native journal; relocation and recovery need their own qualification. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/EXT2.so` and `/System/Resources/Documentation/Filesystem-EXT2.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-EXT2/actions/runs/45534075017) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-EXT2/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/ext2_common.c)
- [Native design](native/filesystems/ext2/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/EXT2.pages) and [PDF](docs/EXT2.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.
