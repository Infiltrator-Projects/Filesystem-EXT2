# SPDX-License-Identifier: GPL-3.0-or-later
# CMake owns the installed V2-only EXT2.so. This Makefile additionally tests
# the same native engine in a freestanding, no-import binary for host adapters.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
OUT ?= build
NATIVE := native/filesystems/ext2
CORE := $(NATIVE)/core/ext2_core.c $(NATIVE)/core/ext2_engine.c
ENGINE := $(NATIVE)/module/ext2_module.c
INCS := -I$(NATIVE)/core -I$(NATIVE)/module
.PHONY: all test install clean native-test
all: $(OUT)/EXT2.so
$(OUT):
	mkdir -p "$(OUT)"
$(OUT)/EXT2.so: CMakeLists.txt VERSION module/ext2_common.c $(CORE) $(ENGINE) | $(OUT)
	cmake -S . -B "$(OUT)/cmake" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
	cmake --build "$(OUT)/cmake" --target ext-ext2-driver --parallel 2
	cp "$(OUT)/cmake/EXT2.so" "$@"
$(OUT)/EXT2-engine.so: $(CORE) $(ENGINE) $(NATIVE)/module/ext2_module.h | $(OUT)
	$(CC) $(CFLAGS) $(INCS) -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fvisibility=hidden \
	  -nostdlib -shared -Wl,-z,defs -Wl,-z,relro -Wl,-z,now \
	  $(CORE) $(ENGINE) -o "$@"
$(OUT)/ext2-native-test: tests/test_ext2_native_engine.c $(CORE) $(ENGINE) | $(OUT)
	$(CC) $(CFLAGS) $(INCS) $^ -o "$@"
native-test: $(OUT)/EXT2-engine.so $(OUT)/ext2-native-test
	"$(OUT)/ext2-native-test"
	@if readelf -d "$(OUT)/EXT2-engine.so" | grep -q '[(]NEEDED[)]'; then \
	  echo 'Freestanding EXT2 engine unexpectedly imports host libraries' >&2; exit 1; fi
test: native-test
	cmake -S . -B "$(OUT)/cmake" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
	cmake --build "$(OUT)/cmake" --parallel 2
	ctest --test-dir "$(OUT)/cmake" --output-on-failure
	cp "$(OUT)/cmake/EXT2.so" "$(OUT)/EXT2.so"
	bash tests/check_v2_exports.sh "$(OUT)/EXT2.so"
install: $(OUT)/EXT2.so
	@test "$$(head -c 5 docs/EXT2.pdf)" = '%PDF-'
	install -Dm0644 "$(OUT)/EXT2.so" "$(DESTDIR)/System/Filesystems/EXT2.so"
	install -Dm0644 "docs/EXT2.pdf" "$(DESTDIR)/System/Resources/Documentation/Filesystem-EXT2.pdf"
clean:
	rm -rf "$(OUT)"
