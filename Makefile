CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
BASE := native/filesystems/ext2
CORE := $(BASE)/core/ext2_core.c $(BASE)/core/ext2_engine.c
MODULE := $(BASE)/module/ext2_module.c
INCS := -I$(BASE)/core -I$(BASE)/module
OUT ?= build
# The disposable training build may link gcov; final EXT2.so remains freestanding.
ifeq ($(PROFILE_TRAIN),1)
  FREESTANDING_LINK :=
else
  FREESTANDING_LINK := -nostdlib
endif
.PHONY: all test install clean
all: $(OUT)/EXT2.so
$(OUT):
	mkdir -p "$(OUT)"
$(OUT)/EXT2.so: $(CORE) $(MODULE) $(BASE)/module/ext2_module.h $(BASE)/core/ext2_engine.h | $(OUT)
	$(CC) $(CFLAGS) $(INCS) -fPIC -ffreestanding -fno-builtin -fno-stack-protector -fvisibility=hidden $(FREESTANDING_LINK) -shared -Wl,-z,defs -Wl,-z,relro -Wl,-z,now $(CORE) $(MODULE) -o "$@"
$(OUT)/ext2-test: tests/ext2_dropin_module_test.c $(OUT)/EXT2.so | $(OUT)
	$(CC) $(CFLAGS) $(INCS) $< $(OUT)/EXT2.so -Wl,-rpath,'$$ORIGIN' -o "$@"
test: $(OUT)/EXT2.so $(OUT)/ext2-test
	$(OUT)/ext2-test
	readelf -Ws $(OUT)/EXT2.so | grep -F infiltrator_filesystem_module_v1
	readelf -p .ifsmeta $(OUT)/EXT2.so | grep -F 'IFS-MODULE:ext2:1'
	@if [ "$(PROFILE_TRAIN)" != "1" ]; then ! readelf -d $(OUT)/EXT2.so | grep -q '[(]NEEDED[)]'; fi
# Installation payload is exactly EXT2.so and one illustrated PDF.
# DESTDIR stages an isolated package without modifying the host filesystem.
install: $(OUT)/EXT2.so
	@test "$$(head -c 5 docs/EXT2.pdf)" = '%PDF-'
	install -Dm0644 "$(OUT)/EXT2.so" "$(DESTDIR)/System/Filesystems/EXT2.so"
	install -Dm0644 "docs/EXT2.pdf" "$(DESTDIR)/System/Resources/Documentation/Filesystem-EXT2/EXT2.pdf"
clean:
	rm -rf "$(OUT)"
