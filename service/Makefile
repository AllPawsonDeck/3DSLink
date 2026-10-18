# 3DSLink background service -> build/service/3DSLinkService.cia
ROOT      := ..
BUILD     := $(ROOT)/build/service
TARGET    := $(BUILD)/3DSLinkService
VERSION   := $(shell cat $(ROOT)/VERSION)
V_MAJOR   := $(word 1,$(subst ., ,$(VERSION)))
V_MINOR   := $(word 2,$(subst ., ,$(VERSION)))
V_MICRO   := $(word 3,$(subst ., ,$(VERSION)))

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
CTRULIB   ?= $(DEVKITPRO)/libctru
PORTLIBS  ?= $(DEVKITPRO)/portlibs/3ds
CC        := $(DEVKITARM)/bin/arm-none-eabi-gcc
MAKEROM   ?= makerom

ARCH      := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS    := -std=gnu11 -g -O2 -Wall -Wextra -Wno-unused-parameter -mword-relocations \
             -ffunction-sections -fdata-sections $(ARCH) -D__3DS__ \
             -DTDSL_VERSION=\"$(VERSION)\" -Isource -I$(PORTLIBS)/include -I$(CTRULIB)/include
LDFLAGS   := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(TARGET).map
LIBS      := -L$(PORTLIBS)/lib -L$(CTRULIB)/lib -ljpeg -lctru -lm

SRCS      := $(wildcard source/*.c source/core/*.c)
OBJS      := $(patsubst source/%.c,$(BUILD)/obj/%.o,$(SRCS)) $(BUILD)/obj/web_index.o

all: $(TARGET).cia

$(BUILD)/web_index.c: $(ROOT)/web/index.html $(ROOT)/tools/embed_web.py
	@mkdir -p $(dir $@)
	python3 $(ROOT)/tools/embed_web.py $< $@

$(BUILD)/obj/web_index.o: $(BUILD)/web_index.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/%.o: source/%.c $(wildcard source/*.h source/core/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(TARGET).cia: $(TARGET).elf service.rsf $(ROOT)/VERSION
	$(MAKEROM) -f cia -o $@ -elf $< -rsf service.rsf -target t -ignoresign \
		-major $(V_MAJOR) -minor $(V_MINOR) -micro $(V_MICRO)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
