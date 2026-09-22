#!/bin/bash
# Build spec_power_p2_v11.ko on the Pi5.
#   *** DRAFT — review src/spec_power_p2_v11.c asm before insmod. ***
#
# Source must already be synced to the Pi, e.g. from the workstation:
#   scp experiments/phase2/src/spec_power_p2_v11.c \
#       pi-min5:/home/pi/spectre_multi/phase2/src/
# then run this script on the Pi (or via: ssh pi-min5 'bash -s' < build_v11_pi.sh).
set -e
cd /home/pi/spectre_multi/phase2
sudo rmmod spec_power_p2_v11 2>/dev/null || true
cp src/spec_power_p2_v11.c spec_power_p2_v11.c
cat > Kbuild <<'EOF'
obj-m += spec_power_p2_v11.o
ccflags-y += -Wall -Wno-declaration-after-statement
EOF
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules 2>&1 | tail -8
ls -la spec_power_p2_v11.ko
echo "v11 BUILT"
