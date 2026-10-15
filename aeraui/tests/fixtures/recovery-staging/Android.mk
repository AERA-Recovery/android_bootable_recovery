# Symlink for file_contexts
include $(CLEAR_VARS)

LOCAL_MODULE := file_contexts_text
LOCAL_MODULE_TAGS := optional
LOCAL_REQUIRED_MODULES := file_contexts.bin

LOCAL_POST_INSTALL_CMD := \
     $(hide) cp ${SOONG_OUT_DIR}/.intermediates/system/sepolicy/file_contexts.concat.tmp/android_common/gen/file_contexts.concat.tmp $(TARGET_RECOVERY_ROOT_OUT)/file_contexts && cp $(PRODUCT_OUT)/obj/ETC/file_contexts.bin_intermediates/file_contexts.bin $(TARGET_RECOVERY_ROOT_OUT)/;

# Darth9
#
# make sure that the nano config directory is copied for nano
ifeq ($(OF_USE_NANO_EDITOR),1)
	LOCAL_POST_INSTALL_CMD += \
	mkdir -p $(TARGET_OUT_ETC)/; \
	mkdir -p $(TARGET_RECOVERY_ROOT_OUT)/system/etc/; \
	cp -rf $(TARGET_OUT_ETC)/nano $(TARGET_RECOVERY_ROOT_OUT)/system/etc/;
endif

# Ship the terminfo database so terminal apps (vi/htop/less/...) work in the
# AERA terminal, which advertises TERM=xterm-256color.
# This is independent of the nano editor; copy it whenever the source exists.
ifneq ($(wildcard external/libncurses/lib/terminfo),)
	LOCAL_POST_INSTALL_CMD += \
	mkdir -p $(TARGET_RECOVERY_ROOT_OUT)/system/etc/; \
	rm -rf $(TARGET_RECOVERY_ROOT_OUT)/system/etc/terminfo; \
	cp -rf external/libncurses/lib/terminfo $(TARGET_RECOVERY_ROOT_OUT)/system/etc/;
endif

# deal with "cannot delete non-empty directory: root/vendor" errors
ifeq ($(OF_MANUAL_ROOT_VENDOR_ERROR_FIX),1)
LOCAL_POST_INSTALL_CMD += \
        rm -f $(TARGET_RECOVERY_ROOT_OUT)/../../root/vendor; \
        mkdir -p $(TARGET_RECOVERY_ROOT_OUT)/../../root/vendor/;
endif
#
# Darth9

include $(BUILD_PHONY_PACKAGE)
