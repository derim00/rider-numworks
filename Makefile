Q ?= @
CC = arm-none-eabi-gcc
BUILD_DIR = build
NWLINK = npx --yes -- nwlink@1.0.0

src = src/main_eadk.c src/game.c

CFLAGS = -std=gnu11
CFLAGS += $(shell $(NWLINK) eadk-cflags-device)
CFLAGS += -O2 -Wall -fno-math-errno -ffast-math
CFLAGS += -fdata-sections -ffunction-sections
CFLAGS += -flto -fno-fat-lto-objects -fwhole-program -fvisibility=internal
LDFLAGS = $(shell $(NWLINK) eadk-ldflags-device)
LDFLAGS += --specs=nano.specs -lm
LDFLAGS += -Wl,-e,main -Wl,-u,eadk_app_name -Wl,-u,eadk_app_icon -Wl,-u,eadk_api_level
LDFLAGS += -Wl,--gc-sections -flinker-output=nolto-rel

.PHONY: build
build: $(BUILD_DIR)/rider.nwa

.PHONY: check
check: $(BUILD_DIR)/rider.bin

.PHONY: run
run: $(BUILD_DIR)/rider.nwa
	$(Q) $(NWLINK) install-nwa $<

.PHONY: assets
assets:
	python3 tools/gen_assets.py
	python3 tools/gen_levels.py

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.nwa
	@echo "BIN     $@"
	$(Q) $(NWLINK) nwa-bin $< $@

$(BUILD_DIR)/%.elf: $(BUILD_DIR)/%.nwa
	@echo "ELF     $@"
	$(Q) $(NWLINK) nwa-elf $< $@

$(BUILD_DIR)/rider.nwa: $(src:%.c=$(BUILD_DIR)/%.o) $(BUILD_DIR)/icon.o
	@echo "LD      $@"
	$(Q) $(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/%.o: %.c src/assets.h src/levels.h src/platform.h
	@echo "CC      $<"
	$(Q) mkdir -p $(@D)
	$(Q) $(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/icon.o: src/icon.png
	@echo "ICON    $<"
	$(Q) mkdir -p $(@D)
	$(Q) $(NWLINK) png-icon-o $< $@

# PC test harness (bot + screenshots)
$(BUILD_DIR)/host: host/host.c src/game.c src/assets.h src/levels.h
	mkdir -p $(@D)
	gcc -O2 -Wall -o $@ host/host.c -lm

.PHONY: test
test: $(BUILD_DIR)/host
	./$(BUILD_DIR)/host bot

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
