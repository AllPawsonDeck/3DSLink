# Builds the streaming core for the PC so it can be tested without a 3DS.
ROOT    := ..
BUILD   := $(ROOT)/build/host
VERSION := $(shell cat $(ROOT)/VERSION)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wno-unused-parameter -DTDSL_HOST -DTDSL_VERSION=\"$(VERSION)\" \
           -D_DEFAULT_SOURCE -I$(ROOT)/service/source
LDLIBS  := -ljpeg

SRCS := host_platform.c $(wildcard $(ROOT)/service/source/core/*.c) $(BUILD)/web_index.c

$(BUILD)/3dslink-host: $(SRCS) $(wildcard $(ROOT)/service/source/core/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(BUILD)/web_index.c: $(ROOT)/web/index.html $(ROOT)/tools/embed_web.py
	python3 $(ROOT)/tools/embed_web.py $< $@

clean:
	rm -rf $(BUILD)

.PHONY: clean
