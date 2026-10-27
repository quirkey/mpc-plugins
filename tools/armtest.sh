#!/bin/bash
# Runs inside arm32v7/gcc:11-bullseye (see build.sh armtest), with the repo mounted at /b.
# Builds the toolkit's host_test.c + wrapper + the port's sources with the device's compiler and flags
# and runs it on QEMU's cortex-a15 (ARMv7-A + VFPv4/NEON, like the MPC's Cortex-A17): QEMU's default CPU
# accepts ARMv8 instructions that kill a Gen1 MPC with SIGILL. No sanitizers (ASan doesn't run under QEMU).
#   armtest.sh PORT "SOURCES" "CFLAGS" "ARMFLAGS"
PORT=$1; SOURCES=$2; CFLAGS=$3; XF=$4
OUT=/tmp/armtest; mkdir -p $OUT; OBJS=""
cd /b || exit 1
for f in $SOURCES mpc-vst-plugins/tools/host_test.c mpc-vst-plugins/wrapper/vst2_wrap.c; do
  o=$OUT/$(echo $f | tr / _).o
  case $f in
    *.cpp) g++ -O2 -g -fPIC -std=gnu++11 $CFLAGS $XF -I$PORT/build -Impc-vst-plugins/wrapper -c $f -o $o 2>/dev/null ;;
    */tools/*|*/wrapper/*) gcc -O2 -g -std=gnu11 $XF -I$PORT/build -c $f -o $o 2>/dev/null ;;
    *) gcc -O2 -g -fPIC -std=gnu11 $CFLAGS $XF -I$PORT/build -Impc-vst-plugins/wrapper -c $f -o $o 2>/dev/null ;;
  esac || { echo "FAIL compiling $f"; exit 1; }
  OBJS="$OBJS $o"
done
g++ $OBJS -lm -lpthread -o $OUT/host_test || { echo "FAIL linking"; exit 1; }
$OUT/host_test > $OUT/log 2>&1; rc=$?
grep -v '^ok' $OUT/log | tail -8
[ $rc = 132 ] && echo "FAIL illegal instruction on an ARMv7 CPU: this build would crash a Gen1 MPC (SIGILL)"
exit $rc
