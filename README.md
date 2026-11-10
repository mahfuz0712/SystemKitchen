# MUX // SYSTEM KITCHEN (WSL / Linux)
Unpack a stock ext4 system.img, delete bloat, add your apps, repack to the same size.

## Setup (WSL Ubuntu)
    sudo apt install build-essential e2fsprogs zlib1g-dev libgtk-3-dev pkg-config
    make
## GUI (runs inside WSL via WSLg)
    ./dist/kitchen-gui
## CLI
    ./dist/kitchen            (interactive menu)
    ./dist/kitchen --project p1 unpack /mnt/d/system.img | list | delete <paths> | add [--priv] a.apk | prop fp | repack | export /mnt/d/flash
Windows drives are /mnt/c, /mnt/d ... Keep the project under ~/ (Linux filesystem), not /mnt.
Flash: fastboot --disable-verity --disable-verification flash vbmeta vbmeta.img; fastboot flash system system_custom.img
