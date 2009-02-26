# hardware/libaudio-alsa/Android.mk
#
# Copyright 2008 Wind River Systems
#

ifeq ($(strip $(BOARD_USES_ALSA_AUDIO)),true)

  LOCAL_PATH := $(call my-dir)

  include $(CLEAR_VARS)

  LOCAL_ARM_MODE := arm
  LOCAL_CFLAGS := -D_POSIX_SOURCE
  LOCAL_WHOLE_STATIC_LIBRARIES := libasound

  LOCAL_C_INCLUDES += external/alsa-lib/include

  LOCAL_SRC_FILES := AudioHardwareALSA.cpp

  LOCAL_MODULE := libaudio

  LOCAL_STATIC_LIBRARIES += libaudiointerface

  LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    libmedia \
    libhardware_legacy \
    libdl \
    libc

  include $(BUILD_SHARED_LIBRARY)

endif
