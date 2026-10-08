#!/bin/sh
# On-target smoke of the YOFO Studio backend on the PZ7035 PS (impl spec S6).
# Run from a directory holding the linux-armv7-yocto build outputs (mib_backend_tests,
# mib_backend_smoke_test, yofo_preview_soak); deploy with scripts/yofo/deploy_target.sh.
#
#   target_smoke.sh [SOAK_SECONDS]
#
# 1. Backend and Aravis lifecycle tests from the CTest runner (mock camera, Aravis Fake device).
# 2. yofo_preview_soak against the live PZ7035 GenTL producer (UIO backend, needs root): the
#    512x96 preview at 1 kHz, then the 830 Hz full-field Overview, SOAK_SECONDS each (default 20).
# Prints PASS/FAIL per step and exits non-zero on any failure.
set -u
cd "$(dirname "$0")"
soak=${1:-20}
fail=0
step() {
    name=$1; shift
    start=$(date +%s)
    if "$@" > "log_$name.txt" 2>&1; then r=PASS; else r=FAIL; fail=1; fi
    echo "$r $name ($(( $(date +%s) - start )) s)"
}
for t in backend_lifecycle_smoke_test experiment_coordinator_test mock_camera_smoke_test \
         processing_pipeline_smoke_test aravis_camera_test aravis_capture_lifecycle_test \
         aravis_appbackend_source_test aravis_pipeline_e2e; do
    step "$t" ./mib_backend_tests "$t"
done
step backend_smoke ./mib_backend_smoke_test
export GENICAM_GENTL32_PATH=${GENICAM_GENTL32_PATH:-/usr/lib/genicam}
step soak_preview ./yofo_preview_soak --region 152,256,512,96 --fps 1000 --exposure 900 \
    --seconds "$soak" --interval 5
step soak_overview ./yofo_preview_soak --region 0,0,816,624 --fps 830 --exposure 900 \
    --seconds "$soak" --interval 5
grep -h '"event":"summary"\|"event":"session"' log_soak_*.txt
exit $fail
