#!/usr/bin/env bash
# Turn a staged Studio package (scripts/yofo/package_studio.sh) into the YOFO Studio instrument bundle:
# the PS software plus the PL image, firmware, Linux boot set, producer and slot tools it was
# qualified with, listed in one MD5SUMS and named by one BUILD_INFO line.
#
#   [PZ7035_REF=pl-results9] [PZ_RING_FRAMES=5000 PZ_BOOTARGS='... mem=720M'] scripts/yofo/bundle_assemble.sh PKG_DIR
#
# PZ_RING_FRAMES (results13+, #649): the frames the PL's every-frame ring keeps. The unit gets MIB_PZ_RING_FRAMES, and the
# assembly refuses a bundle whose boot args leave no room for that ring above Linux (mem= must not reach the ring base,
# which ends at the PL's 0x3F000000 area): the two numbers are set together, in the same bundle and BUILD_INFO.
#
# Layout (see docs/exec-plans/active/2026-10-08-yofo-studio-bundle.md):
#   board side  yofo-studio-server dist.tar yofo-studio.service install.sh pl-ready.sh BUILD_INFO MD5SUMS
#               core.json  (-> /etc/yofo/expected-core.json)      producer/libpz7035_gentl.cti
#               tools/{pzcell,pzres,pzpump,page.bin,lut.bin}
#   host side   host/pl/{pz_live.bit,ps7_init.tcl}  host/firmware/live_server.elf
#               host/linux/{pz7035-live-uio.dtb,<rootfs>.cpio.gz.u-boot,bootargs}
# The restore script copies only the board side to the RAM root (the host side is 70 MB) and
# programs the PL, firmware and Linux from host/. Sources default to this host's locations; every
# one can be overridden with the PZ_* variables below.
set -euo pipefail
pkg=${1:?usage: bundle_assemble.sh PKG_DIR}
[ -f "$pkg/BUILD_INFO" ] || { echo "bundle: $pkg is not a staged package (no BUILD_INFO)" >&2; exit 1; }
ref=${PZ7035_REF:-pl-results9}
repo=${PZ7035_REPO:-/home/gavin/Developer/pz7035-imx426}
name=${PZ_PL_IMAGE:-${ref#pl-}}
dev=/mnt/hdd/developer-data/IMX426
cti=${PZ_CTI:-$dev/gentl/main-0f67861-retime/libpz7035_gentl.cti}
cti_md5=${PZ_CTI_MD5:-bc89cd389dbc0ccfa2400e1e5dc0512f}
# The pz7035-imx426 commit the producer (.cti) was built from, named in BUILD_INFO next to the PL tag; checked to exist in the repo.
# Unset: the producer of the earlier bundles (main 0f67861d retime).
cti_commit=${PZ_CTI_COMMIT:-}
fw=${PZ_FIRMWARE:-/home/gavin/Developer/.worktrees/pz7035-mask-stage/build/pz_live_firmware/live_server.elf}
linux_repo=${PZ_LINUX_REPO:-/home/gavin/Developer/pz7035-linux}
dtb=${PZ_DTB:-$dev/linux-boot-20260930/pz7035-live-uio.dtb}
rootfs=${PZ_ROOTFS:-$linux_repo/build/pz7035-fh/tmp/deploy/images/pz7035-fh/yofo-image-pz7035-fh.rootfs.cpio.gz.u-boot}
bootargs=${PZ_BOOTARGS:-uio_pdrv_genirq.of_id=generic-uio mem=1008M}
ring_frames=${PZ_RING_FRAMES:-0}
tools=${PZ_TOOLS_DIR:-/home/gavin/Developer/.worktrees/pz7035-yofo-host-if/tools}
pzpump=${PZ_PZPUMP:-$dev/pump-tushui-20261004/pzpump}
slot=${PZ_SLOT_DATA:-$dev/results-hw-20261005/results6}
die() { echo "bundle: $*" >&2; exit 1; }
need() { [ -f "$1" ] || die "missing $2: $1"; }

# 1. The pz7035 ref and its PL build (bitstream, ps7_init, core identity).
commit=$(git -C "$repo" rev-parse --verify "$ref^{commit}" 2>/dev/null) || die "$ref is not a commit/tag in $repo"
build=${PZ_PL_BUILD:-}
if [ -z "$build" ]; then
    for tree in $(git -C "$repo" worktree list --porcelain | sed -n 's/^worktree //p'); do
        if [ -f "$tree/build/pz_live_$name/pz_live.bit" ] && [ -f "$tree/build/pz_live_$name/core.json" ]; then build=$tree/build/pz_live_$name; break; fi
    done
fi
[ -n "$build" ] || die "no build/pz_live_$name (pz_live.bit + core.json) in any $repo worktree; set PZ_PL_BUILD"
need "$build/pz_live.bit" "bitstream"
need "$build/core.json" "core.json"
ps7=$(ls "$build"/pz_live.gen/sources_1/bd/*/ip/*/ps7_init.tcl 2>/dev/null | head -n 1 || true)
[ -n "$ps7" ] || die "no ps7_init.tcl under $build/pz_live.gen"
read -r core_image build_id abi < <(python3 -c '
import json, sys
c = json.load(open(sys.argv[1]))
print(c["image"], c["build_id"], "%d.%d" % (c["abi"]["major"], c["abi"]["minor"]))' "$build/core.json")
[ "$core_image" = "pz_live_$name" ] || die "$build/core.json is for $core_image, not pz_live_$name"

# 2. Firmware, producer, Linux boot set, slot tools.
need "$fw" "firmware"; need "$cti" "producer"; need "$dtb" "device tree"; need "$rootfs" "RAM root"
cti_note="pz7035-imx426 main 0f67861d retime"
if [ -n "$cti_commit" ]; then
    cti_full=$(git -C "$repo" rev-parse --verify "$cti_commit^{commit}" 2>/dev/null) || die "PZ_CTI_COMMIT $cti_commit is not a commit in $repo"
    cti_note="pz7035-imx426 ${cti_full:0:8}"
fi
[ "$(md5sum "$cti" | cut -d' ' -f1)" = "$cti_md5" ] || die "producer md5 is not $cti_md5"
for f in pzcell/pzcell pzres/pzres; do need "$tools/$f" "slot tool"; done
need "$pzpump" "pzpump"; need "$slot/page.bin" "page.bin"; need "$slot/lut.bin" "lut.bin"
src_of() { # source commit of a file's repository, with a dirty marker
    local dir top; dir=$(dirname "$(readlink -f "$1")")
    top=$(git -C "$dir" rev-parse --show-toplevel 2>/dev/null) || { echo "not in git"; return; }
    echo "$(git -C "$top" rev-parse --short=8 HEAD)$([ -n "$(git -C "$top" status --porcelain --untracked-files=no)" ] && echo ' (dirty)')"
}
fw_commit=$(src_of "$fw"); fw_note=""
fw_full=$(git -C "$(dirname "$fw")" rev-parse HEAD 2>/dev/null || true)
if [ -n "$fw_full" ] && ! git -C "$repo" merge-base --is-ancestor "$fw_full" "$commit" 2>/dev/null; then
    fw_note=" (NOT an ancestor of $ref: check the firmware)"
fi

# 2b. The frame ring and the boot args must agree (the PL does not enforce a DDR floor; Studio checks /proc/iomem as well).
ring_note=""
if [ "$ring_frames" != 0 ]; then
    ring_note=$(python3 - "$ring_frames" "$bootargs" <<'PY' || exit 1
import re, sys
frames, args = int(sys.argv[1]), sys.argv[2]
m = re.search(r"(?:^|\s)mem=(\d+)([KMG]?)", args)
if not m:
    sys.exit("bundle: PZ_RING_FRAMES needs a mem= in the boot args (Linux must stop below the ring)")
mem = int(m.group(1)) * {"": 1, "K": 1 << 10, "M": 1 << 20, "G": 1 << 30}[m.group(2)]
RECORD, CEILING, FLOOR = 59392, 0x3F000000, 0x00100000
size = frames * RECORD
base = (CEILING - size) // 4096 * 4096
if frames <= 0 or base < FLOOR:
    sys.exit(f"bundle: a ring of {frames} frames does not fit the DDR")
if base < mem:
    sys.exit(f"bundle: boot args mem={m.group(1)}{m.group(2)} reach 0x{mem:08x} but a ring of {frames} frames ({size / 2**20:.1f} MiB) starts at 0x{base:08x}: "
             f"lower mem= to at most {base >> 20} MiB (for example mem={base >> 20}M) or keep fewer frames")
print(f"frame ring: {frames} frames ({size / 2**20:.1f} MiB) at 0x{base:08x}-0x{base + size:08x}, Linux limited by mem={m.group(1)}{m.group(2)} (ends 0x{mem:08x}), unit MIB_PZ_RING_FRAMES={frames}")
PY
)
    grep -q '^\[Service\]' "$pkg/yofo-studio.service" || die "the staged unit has no [Service] section"
fi

# 3. Copy.
rm -rf "$pkg/host" "$pkg/producer" "$pkg/tools"
install -d "$pkg/host/pl" "$pkg/host/firmware" "$pkg/host/linux" "$pkg/producer" "$pkg/tools"
install -m 0644 "$build/pz_live.bit" "$pkg/host/pl/pz_live.bit"
install -m 0644 "$ps7" "$pkg/host/pl/ps7_init.tcl"
install -m 0755 "$fw" "$pkg/host/firmware/live_server.elf"
install -m 0644 "$dtb" "$pkg/host/linux/$(basename "$dtb")"
install -m 0644 "$(readlink -f "$rootfs")" "$pkg/host/linux/$(basename "$rootfs")"
printf '%s\n' "$bootargs" > "$pkg/host/linux/bootargs"
install -m 0644 "$build/core.json" "$pkg/core.json"
install -m 0644 "$cti" "$pkg/producer/libpz7035_gentl.cti"
install -m 0755 "$tools/pzcell/pzcell" "$tools/pzres/pzres" "$pzpump" "$pkg/tools/"
install -m 0644 "$slot/page.bin" "$slot/lut.bin" "$pkg/tools/"

if [ "$ring_frames" != 0 ]; then
    # one line, set together with the boot args above
    sed -i '/^Environment=MIB_PZ_RING_FRAMES=/d' "$pkg/yofo-studio.service"
    sed -i "/^\[Service\]/a Environment=MIB_PZ_RING_FRAMES=$ring_frames" "$pkg/yofo-studio.service"
fi

# 4. BUILD_INFO: one line names the bundle (mib + pz7035 commits, PL BUILD_ID, ABI); the rest says where each part came from.
mib_commit=$(sed -n '1s/.*commit \([0-9a-f]*\).*/\1/p' "$pkg/BUILD_INFO")
[ -n "$mib_commit" ] || die "cannot read the mib-studio-qt commit from $pkg/BUILD_INFO"
pz_short=$(printf '%.8s' "$commit")
{
    untagged=""
    git -C "$repo" show-ref --verify --quiet "refs/tags/$ref" || untagged=" -- PRE-QUALIFICATION, untagged pz7035 $pz_short"
    echo "YOFO Studio bundle: mib-studio-qt $mib_commit + pz7035-imx426 $pz_short ($ref), PL BUILD_ID ${build_id:0:8}, ABI $abi$untagged"
    sed '1d' "$pkg/BUILD_INFO" | grep -v '^expects:'
    echo "pz7035-imx426: $ref = $commit"
    echo "PL: $name BUILD_ID $build_id, ABI $abi, bitstream md5 $(md5sum "$build/pz_live.bit" | cut -d' ' -f1) (core.json image pz_live_$name; installed as /etc/yofo/expected-core.json)"
    echo "firmware: live_server.elf from commit $fw_commit$fw_note"
    echo "linux: $(basename "$dtb") + $(basename "$rootfs"), pz7035-linux $(git -C "$linux_repo" rev-parse --short=8 HEAD 2>/dev/null || echo "not in git"), boot args: $bootargs"
    [ -z "$ring_note" ] || echo "$ring_note"
    echo "producer: libpz7035_gentl.cti md5 $cti_md5 ($cti_note)"
    echo "tools: pzcell, pzres from $(src_of "$tools/pzcell/pzcell"), pzpump, page.bin, lut.bin from $(basename "$slot")"
    echo "boot check: Studio's preflight compares the loaded PL's BUILD_ID with core.json from this bundle; a mismatch means the wrong image is loaded"
} > "$pkg/BUILD_INFO.new"
mv "$pkg/BUILD_INFO.new" "$pkg/BUILD_INFO"

# 5. One MD5SUMS for everything.
(cd "$pkg" && find . -type f ! -name MD5SUMS | sed 's|^\./||' | LC_ALL=C sort | xargs md5sum > MD5SUMS)
echo "bundle $pkg: $(head -n 1 "$pkg/BUILD_INFO")"
