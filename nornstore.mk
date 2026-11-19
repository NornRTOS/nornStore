# nornStore as a module of a nornOS application. The application's app.mk says
#
#   MODULES += $(NORN_RTOS)/modules/nornStore/nornstore.mk
#
# and the nornOS Makefile includes this file, which appends to MOD_SRC, MOD_INC and
# MOD_CFLAGS. Every source under src/ is compiled; what the application does not call is
# dropped by the linker (--gc-sections), and an application that never calls store_init()
# gets no byte of nornStore. A store_config.h next to the application overrides the
# STORE_CFG_* defaults of store.h.
STORE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# The `config` command for nosh is compiled only when nosh.mk is among the application's
# MODULES: it includes nosh.h, and an application without the shell has no use for it.
MOD_SRC += $(filter-out %/store_cmd_config.c,$(wildcard $(STORE_DIR)/src/*.c))
ifneq ($(filter %/nosh.mk,$(MODULES)),)
  MOD_SRC += $(STORE_DIR)/src/store_cmd_config.c
endif
MOD_INC += -I$(STORE_DIR)/include
ifneq ($(wildcard $(APPDIR)/store_config.h),)
  MOD_CFLAGS += -DSTORE_CONFIG_H
endif
