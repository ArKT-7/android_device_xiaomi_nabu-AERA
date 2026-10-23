# AERA build environment setup for Xiaomi Pad 5 (nabu)

FDEVICE="nabu"

aera_get_target_device() {
	export script_path="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
	if echo "$script_path" | grep -q "$FDEVICE"; then
		AERA_BUILD_DEVICE="$FDEVICE"
	elif echo "$0" | grep -q "$FDEVICE"; then
		AERA_BUILD_DEVICE="$FDEVICE"
	fi
}

if [ -z "$AERA_BUILD_DEVICE" ]; then
	aera_get_target_device
fi

if [ "$AERA_BUILD_DEVICE" = "$FDEVICE" ]; then
	echo "Detected build device: $AERA_BUILD_DEVICE"

	export AERA_BUILD_STATUS=Unofficial
	export AERA_BUILD_TYPE=Alpha
	export AERA_VANILLA_BUILD=1
	export AERA_VIRTUAL_AB_DEVICE=1
	export AERA_USE_BASH_SHELL=1
	export AERA_USE_NANO_EDITOR=1
	export AERA_USE_TAR_BINARY=1
	export AERA_USE_LZ4_BINARY=1
	export AERA_USE_SED_BINARY=1
	export AERA_USE_XZ_UTILS=1
	export AERA_USE_ZSTD_BINARY=1
	export AERA_REMOVE_AAPT=1
	export AERA_USE_BUSYBOX_BINARY=1
	export AERA_USE_GREP_BINARY=1
	export AERA_SETTINGS_ROOT_DIRECTORY="/cust"
	export AERA_ALLOW_EARLY_SETTINGS_LOAD=1
	for p in build-make; do
		if patch -p1 -N --dry-run < device/xiaomi/nabu/patches/$p.patch; then
			patch -p1 -N < device/xiaomi/nabu/patches/$p.patch
		fi
	done
else
	echo "$FDEVICE vendorsetup.sh skipped, device mismatch or environment issue"
fi
