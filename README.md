# MUX // SYSTEM KITCHEN (C++, WSL Ubuntu / Linux)

Unpack a stock ext4 `system.img`, delete bloat, add your own apps, repack to the SAME size so it fits the partition.
No root, no mount: it drives e2fsprogs (debugfs / e2fsck / resize2fs / dumpe2fs).

## Build
    sudo apt update && sudo apt install -y g++ make zlib1g-dev e2fsprogs
    make                 # -> dist/kitchen
    ./dist/kitchen       # project in ~/system-kitchen/project1
    ./dist/kitchen ~/kitchen/tab
Keep project folders inside WSL (~/...), not under /mnt/c.

Layout: src/common.* (HUD output, process runner, sparse images, debugfs layer), src/apk.* (zip + Android binary-XML manifest reader),
src/main.cpp (project state + menu actions), build/, dist/.

## Menu
1. Unpack: sparse -> raw ext4, grows it (256 MB, or 800 MB if `shared_blocks`) and unshares the read-only dedup blocks of stock Android 10 images.
2. Info: sizes, free space, build.prop highlights.
3. Apps: size, package, SAFE / RISKY, shared UID, uses-library "needed by". `f safe|risky|all`, `d 3` details, `3,5,10-12` picks, then `yes`.
4. Add apps: /system/app (default) with root:root, 0755/0644, SELinux `u:object_r:system_file:s0`; native libs (arm64/arm) extracted to lib/<abi>/.
   priv-app also writes a privapp-permissions whitelist XML from the manifest (Android 9+ bootloops without it).
5. build.prop: set a property, or `fp` to bump the fingerprint (new system apps then get default permissions on first boot).
6. Repack: e2fsck, shrink back to the original size, out/system_custom.raw.img + out/system_custom.img (sparse) + sha256.
7. Flash guide.

## Flash (bootloader unlocked)
    fastboot --disable-verity --disable-verification flash vbmeta google-vbmeta.img    # or the stock vbmeta, same command
    fastboot flash system system_custom.img
Then factory reset. Keep the stock system / vbmeta / boot images.

## Limits
ext4 system.img only (not EROFS / super.img). The manifest reader is a minimal one (package, sharedUserId, uses-library, uses-permission);
if it fails the app shows package `?` and is classified by folder name only. Tested here on a generated ext4 image, not on the real Lenovo image.
