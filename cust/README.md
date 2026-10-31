# AERA Recovery - Custom Partition Data

This folder contains extra drivers, libraries, and hardware blobs (for GPU) that AERA Recovery requires to operate with full hardware acceleration.

**Why are these files stored here?**
The Xiaomi Pad 5 (nabu) does not have a dedicated recovery partition, instead the recovery lives inside the `boot.img` as a ramdisk right alongside the kernel.

If we pack all these massive hardware binaries directly into that ramdisk, the boot image gets way too big and will eventually fail to flash or boot, to get around this, we store these extra files here in the `/cust` partition (which is a useless ~1GB partition on nabu). 

When AERA recovery boots, it simply mounts `/cust` and loads what it needs on the fly.

### How to Build and Flash `cust.img`

Because this folder needs to be present in the cust partition, run this script to pack it into a flashable `cust.img` file.

1. Open your terminal in this `cust/` directory.
2. Run the build script:
   ```bash
   chmod +x build_cust.sh
   ./build_cust.sh