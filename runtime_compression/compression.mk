# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0
# Opt in from BoardConfig.mk with AERA_USE_EXTREME_COMPRESSION := 1.
# The bootloader still receives the usual outer ramdisk compression. Runtime
# files expand from XZ in RAM before device early-init services are started.
ifeq ($(AERA_USE_EXTREME_COMPRESSION),1)
aera_runtime_compression_path := $(LOCAL_PATH)/runtime_compression
$(PRODUCT_OUT)/ramdisk-recovery.cpio: MKBOOTFS := $(aera_runtime_compression_path)/pack_ramdisk.py
$(PRODUCT_OUT)/ramdisk-recovery.cpio: \
    $(aera_runtime_compression_path)/pack_ramdisk.py \
    $(aera_runtime_compression_path)/aera-runtime-unpack.sh \
    $(aera_runtime_compression_path)/init.recovery.aera-runtime.rc \
    vendor/recovery/prebuilt/$(TARGET_ARCH)/busybox \
    prebuilts/build-tools/linux-x86/bin/xz
endif
