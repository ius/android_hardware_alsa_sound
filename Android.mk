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

  ifneq ($(ALSA_DEFAULT_SAMPLE_RATE),)
    LOCAL_CFLAGS += -DALSA_DEFAULT_SAMPLE_RATE=$(ALSA_DEFAULT_SAMPLE_RATE)
  endif

  ifeq ($(strip $(BOARD_HAVE_FM_ROUTING)),true)
    LOCAL_CFLAGS += -DFM_ROUTE_SUPPORT
  endif

  LOCAL_C_INCLUDES += external/alsa-lib/include

  LOCAL_SRC_FILES := AudioHardwareALSA.cpp

  LOCAL_MODULE := libaudio

  LOCAL_STATIC_LIBRARIES += libaudiointerface

  LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    libmedia \
    libhardware \
    libhardware_legacy \
    libdl \
    libc

  include $(BUILD_SHARED_LIBRARY)

  include $(CLEAR_VARS)

  LOCAL_PRELINK_MODULE := false

  LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

  LOCAL_CFLAGS := -D_POSIX_SOURCE

  LOCAL_C_INCLUDES += external/alsa-lib/include

  LOCAL_SRC_FILES:= acoustics_default.cpp

  LOCAL_SHARED_LIBRARIES := liblog

  LOCAL_MODULE:= acoustics.default

  include $(BUILD_SHARED_LIBRARY)

endif
