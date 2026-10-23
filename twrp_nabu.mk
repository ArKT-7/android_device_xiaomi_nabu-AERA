# AERA recovery product definition for Xiaomi Pad 5 (nabu)

DEVICE_PATH := device/xiaomi/nabu

# Inherit from device.mk configuration
$(call inherit-product, $(DEVICE_PATH)/device.mk)

# Release name
PRODUCT_RELEASE_NAME := nabu

# Device identifier
PRODUCT_DEVICE := nabu
PRODUCT_NAME := twrp_nabu
PRODUCT_BRAND := xiaomi
PRODUCT_MODEL := Xiaomi Pad 5
PRODUCT_MANUFACTURER := Xiaomi

# Assert
TARGET_OTA_ASSERT_DEVICE := nabu