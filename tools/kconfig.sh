#!/bin/sh
# Configure an O= kernel build from a defconfig plus fragments.
#
#   tools/kconfig.sh <srctree> <outdir> <arch> <cross> <cc> <defconfig> <fragment>...
#
# Fails if anything the fragments asked for did not survive dependency
# resolution.

set -e

src=${1:?usage: $0 <srctree> <outdir> <arch> <cross> <cc> <defconfig> <fragment>...}
out=${2:?} arch=${3:?} cross=${4-} cc=${5:?} defconfig=${6:?}
shift 6

# Both have to be absolute. "make -C $src O=$out" runs make after it has
# changed into $src, so a relative $out would land inside the source tree while
# everything below still read $out relative to here — a half-merged .config and
# a stray build directory in a tree that is meant to stay clean.
mkdir -p "$out"
src=$(cd "$src" && pwd)
out=$(cd "$out" && pwd)

kmake() {
	make -C "$src" O="$out" ARCH="$arch" CROSS_COMPILE="$cross" CC="$cc" "$@"
}

kmake "$defconfig"

# -m merges the fragments and stops. merge_config.sh can do the config pass
# itself, but it runs make internally and can only pass CC through the
# environment, where kbuild's own "CC = $(CROSS_COMPILE)gcc" beats it: it looks
# for the unsuffixed cross compiler and dies *after* writing the fragments in,
# leaving a .config quietly missing them.
KCONFIG_CONFIG="$out/.config" "$src/scripts/kconfig/merge_config.sh" \
	-m "$out/.config" "$@"
kmake olddefconfig

# Skipping that make also skips merge_config.sh's own survival report, so check
# here. Two kinds of casualty: what the fragments asked for, and the promptless
# symbols nothing asks for directly — DRM_SCHED and DRM_GEM_DMA_HELPER are only
# ever turned on by another driver's "select", so they are how we learn that the
# etnaviv or logicvc indirection has stopped working.
missing=
for want in $(sed -n 's/^\(CONFIG_[A-Z0-9_]*=.*\)$/\1/p' "$@") \
	    CONFIG_DRM_SCHED=y CONFIG_DRM_GEM_DMA_HELPER=y; do
	grep -qx -- "$want" "$out/.config" || missing="$missing $want"
done
[ -z "$missing" ] || {
	echo "$0: dropped by dependency resolution:$missing" >&2
	exit 1
}
