# Filesystem-EXT2
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/EXT2.so`
- `/System/Resources/Documentation/Filesystem-EXT2.pdf`
- `/System/Kernel/Filesystems/ext2.ko`

The InfiltratorOS Debian package installs these three regular files. Its bundled `ext2.ko` targets `0.1.1` and must never be loaded into a different kernel ABI. The release also includes an explicitly ABI-labeled `.ko` and a SHA-256 manifest. A manual native RUN compiles the shared object and Linux adapter for the running or explicitly selected kernel, generates the PDF, and installs all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.
