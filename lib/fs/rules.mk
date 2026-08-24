LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += $(LOCAL_DIR)/debug.c
MODULE_SRCS += $(LOCAL_DIR)/fs.c
MODULE_SRCS += $(LOCAL_DIR)/shell.c

MODULE_OPTIONS := test

MODULE_DEPS += lib/bio

# how many unreferenced name nodes the layer keeps cached; see fs.c for the
# default (0 on LK_EMBEDDED targets, 64 otherwise)
ifneq ($(FS_NODE_CACHE_SIZE),)
MODULE_DEFINES += FS_NODE_CACHE_SIZE=$(FS_NODE_CACHE_SIZE)
endif

include make/module.mk
