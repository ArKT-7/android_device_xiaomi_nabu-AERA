# AERA recovery build definitions for Xiaomi Pad 5 (nabu)

LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_DEVICE), nabu)
include $(call all-subdir-makefiles,$(LOCAL_PATH))
endif
