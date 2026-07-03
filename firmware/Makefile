SOURCEDIR = src
BUILDDIR = build
INCLUDEDIR = include

# Use local SDCC if available, otherwise fall back to system sdcc
LOCAL_SDCC := $(firstword $(wildcard ../tools/sdcc-*/bin/sdcc))
ifneq ($(LOCAL_SDCC),)
LDCC ?= $(LOCAL_SDCC)
else
LDCC ?= sdcc
endif
CFLAGS := -lstm8 -mstm8

FIRMWARE_IMAGE = $(BUILDDIR)/halo-fw.ihx

DEBUG_FW_IMAGE = $(patsubst $(BUILDDIR)/%.ihx,$(BUILDDIR)/%.elf,$(FIRMWARE_IMAGE))
SOURCES = $(wildcard $(SOURCEDIR)/*.c)
OBJECTS = $(patsubst $(SOURCEDIR)/%.c,$(BUILDDIR)/%.rel,$(SOURCES))

.PHONY: all clean flash debug

all: $(FIRMWARE_IMAGE)

clean:
	@echo "Cleaning up '$(BUILDDIR)' directory"
	rm -rf $(BUILDDIR)

# Use local stm8flash if available
LOCAL_STM8FLASH := $(firstword $(wildcard ../tools/stm8flash/stm8flash ../tools/stm8flash-master/stm8flash))
LOCAL_LIBUSB_DIR := $(firstword $(wildcard $(HOME)/homebrew/lib /opt/homebrew/lib /usr/local/lib))

flash: $(FIRMWARE_IMAGE)
ifneq (, $(shell command -v STVP_CmdLine.exe;))
	STVP_CmdLine.exe -Device=STM8L15xG4 -FileProg=$(FIRMWARE_IMAGE) -verif -no_loop -no_log
else ifneq ($(LOCAL_STM8FLASH),)
ifneq ($(LOCAL_LIBUSB_DIR),)
	DYLD_LIBRARY_PATH=$(LOCAL_LIBUSB_DIR):$$DYLD_LIBRARY_PATH $(LOCAL_STM8FLASH) -cstlinkv2 -p"stm8l151?4" -w $(FIRMWARE_IMAGE)
else
	$(LOCAL_STM8FLASH) -cstlinkv2 -p"stm8l151?4" -w $(FIRMWARE_IMAGE)
endif
else ifneq (, $(shell command -v stm8flash;))
	stm8flash -cstlinkv2 -p"stm8l151?4" -w $(FIRMWARE_IMAGE)
else
	@echo "STVP_CmdLine or stm8flash not found"
endif

debug: CFLAGS += --debug
debug: $(DEBUG_FW_IMAGE)

$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

$(OBJECTS): $(BUILDDIR)/%.rel : $(SOURCEDIR)/%.c | $(BUILDDIR)
	$(LDCC) $(CFLAGS) -I$(INCLUDEDIR) $(LDFLAGS) -c $< -o $@

$(FIRMWARE_IMAGE): $(OBJECTS) | $(BUILDDIR)
	$(LDCC) $(CFLAGS) --out-fmt-ihx $^ -o $@

$(DEBUG_FW_IMAGE): $(OBJECTS) | $(BUILDDIR)
	$(LDCC) $(CFLAGS) --out-fmt-elf $^ -o $@
