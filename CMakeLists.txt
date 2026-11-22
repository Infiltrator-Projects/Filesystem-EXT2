cmake_minimum_required(VERSION 3.20)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FILESYSTEM_VERSION LIMIT_COUNT 1)
project(FilesystemEXT VERSION ${FILESYSTEM_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
 GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG d23a52693fd3212368d327e1c1302e07ae9510ec)
FetchContent_MakeAvailable(infiltratr_common)
# The embedded Common SDK is linked, not installed with filesystem runtime modules.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
# A real freestanding Common V2 reader is compiled from exactly the same
# EXT2 sources as the hosted module; no EFI filesystem-specific adapter.
# Embed its self-contained ELF byte-for-byte into the installed EXT2.so.
set(EXT2_FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware")
set(EXT2_FIRMWARE_ELF "${EXT2_FIRMWARE_DIR}/EXT2.so")
add_custom_command(OUTPUT "${EXT2_FIRMWARE_ELF}"
 COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
  -B "${EXT2_FIRMWARE_DIR}" -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
 COMMAND "${CMAKE_COMMAND}" --build "${EXT2_FIRMWARE_DIR}" --parallel 2
 DEPENDS
  module/firmware/CMakeLists.txt
  module/firmware/runtime.c
  module/ext2_common.c
  native/filesystems/ext2/core/ext2_core.c
  native/filesystems/ext2/core/ext2_engine.c
  native/filesystems/ext2/module/ext2_module.c
  "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
 VERBATIM)
add_custom_target(ext2-firmware-elf DEPENDS "${EXT2_FIRMWARE_ELF}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"${PROJECT_VERSION}\"\n")
set(EXT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/migration/defragmenter/native")
set_source_files_properties("${EXT_SRC}/ext_worker.c" PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_ext_worker_main")
set_source_files_properties("${EXT_SRC}/ext_metadata_worker.c" PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_ext_metadata_main")
add_library(ext-core OBJECT
 "${EXT_SRC}/ext_disk.c"
 "${EXT_SRC}/ext_common.c"
 "${EXT_SRC}/ext_catalog.c"
 "${EXT_SRC}/ext_plan.c"
 "${EXT_SRC}/ext_workspace.c"
 "${EXT_SRC}/ext_worker.c"
 "${EXT_SRC}/ext_metadata_worker.c"
 fixtures/ext_codec.c module/ext_fixture_bridge.c)
target_include_directories(ext-core PRIVATE "${EXT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(ext-core PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ext-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(ext-core PUBLIC InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
# Independently build only the EXT2 variant from source owned by this repo.
# Use one independently owned EXT2 driver, not a generic three-format loop.
 # Keep the genuine freestanding EXT2 drop-in engine alongside the
 # separately owned EXT Test Media/Defragmenter fixture ABI in ONE EXT2.so.
 # Test Media must not reject an otherwise installed EXT2 module.
 # Production .so is V2-only and links one portable engine, no Defragmenter
 # V1 entrypoint, CLI worker or imported legacy host-service adapters.
 add_library(ext-ext2-driver SHARED
  module/ext2_common.c
  native/filesystems/ext2/core/ext2_core.c
  native/filesystems/ext2/core/ext2_engine.c
  native/filesystems/ext2/module/ext2_module.c)
 set_target_properties(ext-ext2-driver PROPERTIES PREFIX "" OUTPUT_NAME "EXT2" SUFFIX ".so"
  C_VISIBILITY_PRESET hidden)
 # Both installed .deb and native .run carry this one ELF carrier, not an
 # alternate firmware-only package. No kernel driver or separate .so is shipped.
 add_dependencies(ext-ext2-driver ext2-firmware-elf)
 set_property(TARGET ext-ext2-driver APPEND PROPERTY LINK_DEPENDS "${EXT2_FIRMWARE_ELF}")
 add_custom_command(TARGET ext-ext2-driver POST_BUILD
  COMMAND "${CMAKE_OBJCOPY}" --add-section
   ".infiltrator_boot=${EXT2_FIRMWARE_ELF}"
   --set-section-flags .infiltrator_boot=readonly
   "$<TARGET_FILE:ext-ext2-driver>"
  VERBATIM)
 target_compile_definitions(ext-ext2-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ext-ext2-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_options(ext-ext2-driver PRIVATE -Wl,-z,defs
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
 set_property(TARGET ext-ext2-driver APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
 target_include_directories(ext-ext2-driver PRIVATE
  "${EXT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
 target_link_libraries(ext-ext2-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
 if(BUILD_TESTING)
  add_executable(ext-ext2-module-test tests/test_module.c)
  target_compile_definitions(ext-ext2-module-test PRIVATE IFS_EXT_FORMAT_ID="ext2")
  target_include_directories(ext-ext2-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
  target_link_libraries(ext-ext2-module-test PRIVATE ext-ext2-driver InfiltratrCommon::Common)
  add_test(NAME ext-ext2-module-contract COMMAND ext-ext2-module-test)
  # Prove the original EXT2 drop-in read/write interface survives the
  # addition of the Test Media fixture and full EXT defrag interface.
  add_executable(ext2-dropin-module-test tests/ext2_dropin_module_test.c)
  target_include_directories(ext2-dropin-module-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext2/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext2/module")
  target_compile_options(ext2-dropin-module-test PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  target_compile_definitions(ext2-dropin-module-test PRIVATE IFS_TEST_COMMON_V2=1)
  # Exercise the native EXT2 engine from the test executable itself. Its
  # private entry points must never be exported by the installed EXT2.so.
  target_sources(ext2-dropin-module-test PRIVATE
    native/filesystems/ext2/core/ext2_core.c
    native/filesystems/ext2/core/ext2_engine.c
    native/filesystems/ext2/module/ext2_module.c)
  target_link_libraries(ext2-dropin-module-test PRIVATE ext-ext2-driver InfiltratrCommon::Common)
  add_test(NAME ext2-dropin-module-contract COMMAND ext2-dropin-module-test)
 endif()
 install(TARGETS ext-ext2-driver LIBRARY DESTINATION /System/Filesystems)
 install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/EXT2.pdf"
         DESTINATION /System/Resources/Documentation RENAME Filesystem-EXT2.pdf)

# The bitmap/descriptor cache regression came from Defragmenter, but the
# private EXT disk implementation and its native unit tests live here.
if(BUILD_TESTING)
 # The historical worker is retained only for independent regression testing;
 # it is NOT linked to or distributed with EXT2.so.
 add_library(ext-legacy-test-support SHARED
  $<TARGET_OBJECTS:ext-core>
  compat/legacy_services.c compat/runtime_alloc.c module/ext_module.c)
 target_include_directories(ext-legacy-test-support PRIVATE
   "${EXT_SRC}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/compat"
   "${CMAKE_CURRENT_BINARY_DIR}")
 target_compile_definitions(ext-legacy-test-support PRIVATE
   _GNU_SOURCE _FILE_OFFSET_BITS=64 IFS_EXT_FORMAT_ID="ext2")
 target_link_libraries(ext-legacy-test-support PRIVATE
   InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
 add_library(ext-test-host STATIC tests/test_host_services.c)
 target_include_directories(ext-test-host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
 target_compile_definitions(ext-test-host PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ext-test-host PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_libraries(ext-test-host PUBLIC ext-legacy-test-support)
 add_executable(ext-bitmap-cache-test tests/test_ext_bitmap_cache.c)
 target_include_directories(ext-bitmap-cache-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/migration/defragmenter/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
 target_compile_definitions(ext-bitmap-cache-test PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
 target_compile_options(ext-bitmap-cache-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 target_link_libraries(ext-bitmap-cache-test PRIVATE ext-test-host InfiltratrCommon::Common OpenSSL::Crypto SQLite::SQLite3 Threads::Threads)
 add_test(NAME ext-bitmap-cache COMMAND ext-bitmap-cache-test)
endif()

if(BUILD_TESTING)
 add_executable(ext2-cross-format-denial-test tests/test_cross_format.c)
 target_include_directories(ext2-cross-format-denial-test PRIVATE
   "${CMAKE_CURRENT_SOURCE_DIR}/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/compat")
 target_link_libraries(ext2-cross-format-denial-test PRIVATE ext-ext2-driver InfiltratrCommon::Common)
 target_compile_options(ext2-cross-format-denial-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 add_test(NAME ext2-rejects-other-formats COMMAND ext2-cross-format-denial-test)
endif()

# Validate the canonical EXT2 group layout independently of Linux VFS headers.
# This is the same core source compiled into EXT2.so and ext2.ko.
if(BUILD_TESTING)
 add_executable(ext2-metadata-layout-test
  tests/test_ext2_metadata_layout.c
  native/filesystems/ext2/core/ext2_core.c)
 target_include_directories(ext2-metadata-layout-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/native/filesystems/ext2/core")
 target_compile_options(ext2-metadata-layout-test PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
 add_test(NAME ext2-metadata-layout COMMAND ext2-metadata-layout-test)
endif()

if(BUILD_TESTING)
 # Build the actual firmware-profile adapter, not the hosted library, so
 # a writable Common V2 host cannot silently activate the native writer.
 add_executable(ext2-firmware-readonly-test
  tests/test_ext2_firmware_readonly.c
  module/ext2_common.c
  native/filesystems/ext2/core/ext2_core.c
  native/filesystems/ext2/core/ext2_engine.c
  native/filesystems/ext2/module/ext2_module.c)
 target_compile_definitions(ext2-firmware-readonly-test PRIVATE
  IFS_EXT2_FIRMWARE_BUILD=1)
 target_include_directories(ext2-firmware-readonly-test PRIVATE
  "${infiltratr_common_SOURCE_DIR}/include")
 target_compile_options(ext2-firmware-readonly-test PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
 target_link_libraries(ext2-firmware-readonly-test PRIVATE
  InfiltratrCommon::Common)
 add_test(NAME ext2-firmware-readonly-lease COMMAND ext2-firmware-readonly-test)
endif()

if(BUILD_TESTING)
 add_test(NAME ext2-no-legacy-public-abi
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_v2_exports.sh"
          "$<TARGET_FILE:ext-ext2-driver>")
endif()
