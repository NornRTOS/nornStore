# nornStore: the module's own checks, run on the PC. The store itself is built into an
# application by nornCore's Makefile through nornstore.mk (see README.md).
#
#   make host-test     tests/test_store.c against nornCore's host port, ASan and UBSan
#   make analyze       gcc -fanalyzer over src/ in the test configuration
#   make size          text/data/bss of every store object for the AVR and the ESP32
#   make clean
#
# NORN_CORE points at the kernel (default: the sibling checkout in the nornRTOS tree).

NORN_CORE ?= ../nornCore
NORN_SHELL ?= ../nornSHELL
CC        ?= gcc
BUILD      = build

SRC       = $(wildcard src/*.c)
CORE_SRC  = $(filter-out src/store_cmd_config.c,$(SRC))     # the store without the nosh command
KERNEL    = $(wildcard $(NORN_CORE)/kernel/*.c) $(wildcard $(NORN_CORE)/port/host/*.c)
INC       = -Iinclude -Itests -I$(NORN_CORE)/kernel -I$(NORN_CORE)/port/host -I$(NORN_SHELL)/include
NOSH_SRC  = $(NORN_SHELL)/src/nosh.c $(NORN_SHELL)/src/nosh_out.c
WARN      = -std=c99 -pedantic -Wall -Wextra -Wshadow -Wundef -Wstrict-prototypes -Wconversion
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer

.PHONY: all host-test analyze size clean

all: host-test

$(BUILD)/test_store: tests/test_store.c tests/rtos_config.h $(CORE_SRC) $(KERNEL) include/*.h src/*.h
	@mkdir -p $(BUILD)
	$(CC) $(WARN) -g -O0 $(SANITIZE) $(INC) -o $@ tests/test_store.c $(CORE_SRC) $(KERNEL)

$(BUILD)/test_store_cmd: tests/test_store_cmd.c tests/rtos_config.h $(SRC) $(KERNEL) $(NOSH_SRC) include/*.h src/*.h
	@mkdir -p $(BUILD)
	$(CC) $(WARN) -g -O0 $(SANITIZE) $(INC) -o $@ tests/test_store_cmd.c $(SRC) $(NOSH_SRC) $(KERNEL)

host-test: $(BUILD)/test_store $(BUILD)/test_store_cmd
	@fail=0; for t in $^; do ASAN_OPTIONS=detect_stack_use_after_return=0 $$t > $$t.log 2>&1; st=$$?; \
	if [ $$st -eq 0 ] && tail -1 $$t.log | grep -q '^PASS'; then tail -1 $$t.log; \
	else echo "FAIL: $$t (exit status $$st)"; tail -20 $$t.log; fail=1; fi; done; exit $$fail

analyze:
	@mkdir -p $(BUILD)/analyze
	@for s in $(SRC); do $(CC) $(WARN) -fanalyzer $(INC) -c $$s -o $(BUILD)/analyze/$$(basename $$s .c).o || exit 1; done
	@echo "analyze: no diagnostics"

# Sizes of the objects alone, every function compiled: what a command costs before the
# linker drops the unused ones. The AVR build takes the UNO, the smallest target.
XTENSA_ROOT ?= $(HOME)/dev/tools/xtensa-esp-elf-16.1.0
size:
	@mkdir -p $(BUILD)/size-avr $(BUILD)/size-esp32
	@for s in $(SRC); do avr-gcc -mmcu=atmega328p -Os $(WARN) -ffunction-sections -fdata-sections \
	    $(INC) -I$(NORN_CORE)/port/avr -I$(NORN_CORE)/hal/avr -I$(NORN_CORE)/hal -DF_CPU=16000000ul \
	    -c $$s -o $(BUILD)/size-avr/$$(basename $$s .c).o || exit 1; done
	@echo "== avr (atmega328p)"; avr-size -t $(BUILD)/size-avr/*.o
	@for s in $(SRC); do $(XTENSA_ROOT)/bin/xtensa-esp32-elf-gcc -mlongcalls -Os $(WARN) -ffunction-sections -fdata-sections \
	    $(INC) -I$(NORN_CORE)/port/xtensa -c $$s -o $(BUILD)/size-esp32/$$(basename $$s .c).o || exit 1; done
	@echo "== esp32"; $(XTENSA_ROOT)/bin/xtensa-esp32-elf-size -t $(BUILD)/size-esp32/*.o

clean:
	rm -rf $(BUILD)
