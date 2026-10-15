ifneq ($(TW_EXCLUDE_NANO), true)
include $(CLEAR_VARS)
LOCAL_MODULE := nano_twrp
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)/system/bin
LOCAL_REQUIRED_MODULES := nano libncurses
LOCAL_POST_INSTALL_CMD += \
    mkdir -p $(TARGET_RECOVERY_ROOT_OUT)/system/etc/; \
    cp -rf $(TARGET_OUT_SYSTEM_EXT_ETC)/nano $(TARGET_RECOVERY_ROOT_OUT)/system/etc/; \
    cp -rf external/libncurses/lib/terminfo $(TARGET_RECOVERY_ROOT_OUT)/system/etc/;
include $(BUILD_PHONY_PACKAGE)
endif

ifeq ($(AERA_BUILD_BASH),1)
	include $(CLEAR_VARS)
	LOCAL_MODULE := bash_fox
	LOCAL_MODULE_TAGS := optional
	LOCAL_MODULE_CLASS := ETC
	LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)/system/bin
	LOCAL_REQUIRED_MODULES := bash

	LOCAL_POST_INSTALL_CMD += \
		mkdir -p $(TARGET_RECOVERY_ROOT_OUT)/system/etc/bash/; \
		cp -rf external/bash/etc/* $(TARGET_RECOVERY_ROOT_OUT)/system/etc/bash/; \
		cp -rf external/libncurses/lib/terminfo $(TARGET_RECOVERY_ROOT_OUT)/system/etc/; \
        sed -i 's/ro.lineage.device/ro.product.device/' $(TARGET_RECOVERY_ROOT_OUT)/system/etc/bash/bashrc; \
        sed -i '/export TERM/d' $(TARGET_RECOVERY_ROOT_OUT)/system/etc/bash/bashrc; \
        mkdir -p $(TARGET_RECOVERY_ROOT_OUT)/sbin/; \
        ln -sf /system/bin/bash $(TARGET_RECOVERY_ROOT_OUT)/sbin/bash;
	include $(BUILD_PHONY_PACKAGE)
endif
