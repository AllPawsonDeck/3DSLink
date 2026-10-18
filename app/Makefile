# 3DSLink app -> build/app/3DSLink.cia (bundles the service CIA in its RomFS)
ROOT      := ..
BUILD     := $(abspath $(ROOT)/build/app)
TARGET    := $(BUILD)/3DSLink
VERSION   := $(shell cat $(ROOT)/VERSION)
V_MAJOR   := $(word 1,$(subst ., ,$(VERSION)))
V_MINOR   := $(word 2,$(subst ., ,$(VERSION)))
V_MICRO   := $(word 3,$(subst ., ,$(VERSION)))
# Same encoding makerom uses for -major/-minor/-micro.
TITLE_VER := $(shell echo $$(( ($(V_MAJOR) << 10) | ($(V_MINOR) << 4) | $(V_MICRO) )))
SERVICE   := $(ROOT)/build/service/3DSLinkService.cia

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB   ?= $(DEVKITPRO)/libctru
CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
MAKEROM   ?= makerom
BANNERTOOL?= bannertool

ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS    := -std=gnu11 -g -O2 -Wall -Wextra -Wno-unused-parameter -mword-relocations \
             -ffunction-sections -fdata-sections $(ARCH) -D__3DS__ \
             -DTDSL_VERSION=\"$(VERSION)\" -DTDSL_TITLE_VERSION=$(TITLE_VER) \
             -Isource -I$(CTRULIB)/include
LDFLAGS   := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(TARGET).map
LIBS      := -L$(CTRULIB)/lib -lctru -lm

SRCS      := source/main.c source/vendor/qrcodegen.c
OBJS      := $(patsubst source/%.c,$(BUILD)/obj/%.o,$(SRCS))

all: $(TARGET).cia

$(SERVICE): FORCE
	$(MAKE) -C $(ROOT)/service

$(BUILD)/romfs/3DSLinkService.cia: $(SERVICE)
	@mkdir -p $(dir $@)
	cp $< $@

# Third-party code: keep its upstream source untouched.
$(BUILD)/obj/vendor/%.o: CFLAGS += -Wno-type-limits

$(BUILD)/obj/%.o: source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(BUILD)/icon.smdh: assets/icon.png
	@mkdir -p $(BUILD)
	$(BANNERTOOL) makesmdh -s "3DSLink" -l "Stream your 3DS screens to a browser" \
		-p "AllPawsonDeck" -i $< -o $@

$(BUILD)/banner.bnr: assets/banner.png assets/banner.wav
	@mkdir -p $(BUILD)
	$(BANNERTOOL) makebanner -i assets/banner.png -a assets/banner.wav -o $@

$(TARGET).cia: $(TARGET).elf app.rsf $(BUILD)/icon.smdh $(BUILD)/banner.bnr \
               $(BUILD)/romfs/3DSLinkService.cia $(ROOT)/VERSION
	$(MAKEROM) -f cia -o $@ -elf $(TARGET).elf -rsf app.rsf -target t -ignoresign \
		-icon $(BUILD)/icon.smdh -banner $(BUILD)/banner.bnr -DAPP_ROMFS=$(BUILD)/romfs \
		-major $(V_MAJOR) -minor $(V_MINOR) -micro $(V_MICRO)

clean:
	rm -rf $(BUILD)

FORCE:
.PHONY: all clean FORCE
