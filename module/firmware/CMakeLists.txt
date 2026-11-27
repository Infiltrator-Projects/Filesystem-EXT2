# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(Ext2BootReader LANGUAGES C)
set(COMMON_INCLUDE_DIR "" CACHE PATH "Pinned Common V2 include root")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h")
  message(FATAL_ERROR "Set COMMON_INCLUDE_DIR to the pinned Common V2 headers")
endif()
# The format core, module adapter and V2 implementation are identical to
# EXT2.so; only the host profile and C runtime surface change.
add_library(ext2-boot MODULE
  ../../module/ext2_common.c
  ../../native/filesystems/ext2/core/ext2_core.c
  ../../native/filesystems/ext2/core/ext2_engine.c
  ../../native/filesystems/ext2/module/ext2_module.c
  runtime.c)
set_target_properties(ext2-boot PROPERTIES
  PREFIX "" OUTPUT_NAME "EXT2" SUFFIX ".so" C_VISIBILITY_PRESET hidden)
target_compile_features(ext2-boot PRIVATE c_std_11)
target_include_directories(ext2-boot PRIVATE "${COMMON_INCLUDE_DIR}")
target_compile_definitions(ext2-boot PRIVATE IFS_EXT2_FIRMWARE_BUILD=1)
# Firmware uses AMD64 SysV without CET, SIMD state assumptions,
# packed relative relocations or non-4-KiB load alignment. Hosted builds
# retain their separate native optimization profile.
target_compile_options(ext2-boot PRIVATE
  -Wall -Wextra -Wpedantic -Werror
  -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns
  -fno-stack-protector -mno-red-zone -fcf-protection=none -mgeneral-regs-only -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(ext2-boot PRIVATE
  -nostdlib -Wl,-z,nopack-relative-relocs,-z,max-page-size=4096,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now)
