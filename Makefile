# brother-scan: universal (Apple Silicon + Intel) build.
#
#   make            build build/brscan and build/stage.noindex/Brother Scan.app
#   make cli        just the command line tool
#   make test       offline decoder tests
#   make install    copy the app to ~/Applications
#   make clean

ARCHS      := arm64 x86_64
MACOS_MIN  := 12.0
BUILD      := build

CC         := clang
SWIFTC     := swiftc
CFLAGS     := -O2 -Wall -Wextra -std=c11 -mmacosx-version-min=$(MACOS_MIN)
FRAMEWORKS := -framework IOKit -framework CoreFoundation -framework CoreGraphics -framework ImageIO

CORE_SRC   := core/usb_iokit.c core/protocol.c core/decode.c core/image_out.c
CORE_HDR   := core/brscan.h core/internal.h
APP_SRC    := app/BrotherScanApp.swift app/ContentView.swift app/Scanner.swift app/ImageHelpers.swift
APP_NAME   := Brother Scan
# Staged in a .noindex folder so Spotlight/Launchpad only show the installed copy.
APP        := $(BUILD)/stage.noindex/$(APP_NAME).app

LSREGISTER := /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

.PHONY: all cli app test install clean

all: cli app

cli: $(BUILD)/brscan
app: $(BUILD)/app.stamp

# ---- per-architecture core library ----
define ARCH_RULES
$(BUILD)/$(1)/%.o: core/%.c $(CORE_HDR)
	@mkdir -p $$(dir $$@)
	$(CC) $(CFLAGS) -arch $(1) -c $$< -o $$@

$(BUILD)/$(1)/libbrscan.a: $(patsubst core/%.c,$(BUILD)/$(1)/%.o,$(CORE_SRC))
	rm -f $$@
	ar rcs $$@ $$^

$(BUILD)/$(1)/brscan: cli/main.c $(BUILD)/$(1)/libbrscan.a
	$(CC) $(CFLAGS) -arch $(1) $$^ $(FRAMEWORKS) -o $$@

$(BUILD)/$(1)/BrotherScan: $(APP_SRC) app/Bridging-Header.h $(BUILD)/$(1)/libbrscan.a
	$(SWIFTC) -O -target $(1)-apple-macos$(MACOS_MIN) \
		-import-objc-header app/Bridging-Header.h \
		$(APP_SRC) $(BUILD)/$(1)/libbrscan.a $(FRAMEWORKS) -o $$@
endef
$(foreach a,$(ARCHS),$(eval $(call ARCH_RULES,$(a))))

# ---- universal binaries ----
$(BUILD)/brscan: $(foreach a,$(ARCHS),$(BUILD)/$(a)/brscan)
	lipo -create $^ -output $@
	codesign -s - -f $@

# The bundle path has a space, which make can't use as a target, so use a stamp.
$(BUILD)/app.stamp: $(foreach a,$(ARCHS),$(BUILD)/$(a)/BrotherScan) app/Info.plist
	rm -rf "$(APP)"
	mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources"
	cp app/Info.plist "$(APP)/Contents/Info.plist"
	lipo -create $(foreach a,$(ARCHS),$(BUILD)/$(a)/BrotherScan) -output "$(APP)/Contents/MacOS/BrotherScan"
	codesign -s - -f "$(APP)"
	touch $@

test: $(BUILD)/test_decode
	$(BUILD)/test_decode

$(BUILD)/test_decode: tests/test_decode.c $(BUILD)/$(firstword $(ARCHS))/libbrscan.a
	$(CC) $(CFLAGS) -arch $(firstword $(ARCHS)) $^ $(FRAMEWORKS) -o $@

install: app
	@mkdir -p "$(HOME)/Applications"
	rm -rf "$(HOME)/Applications/$(APP_NAME).app"
	cp -R "$(APP)" "$(HOME)/Applications/"
	$(LSREGISTER) -f "$(HOME)/Applications/$(APP_NAME).app"
	@echo "Installed to ~/Applications/$(APP_NAME).app"

clean:
	rm -rf $(BUILD)
