import argparse
from datetime import datetime
import json
from pathlib import Path
import sys

from .analysis import Policy
from .workflow import tune


def main(argv=None):
    parser = argparse.ArgumentParser(description="Tune MindVision/LED sync from raw image intensity. Close camera-owning apps first; no desktop/focus/pump automation.")
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--capture", type=Path, help="Path to built mib_sync_capture executable")
    parser.add_argument("--output", type=Path, default=Path("data/sync-tuning") / datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    parser.add_argument("--delays", default="0,5,15,30,45,60,75", help="Comma-separated acquisition delays in us")
    parser.add_argument("--exposures", help="Comma-separated exposures in us; default: saved exposure only")
    parser.add_argument("--sample-seconds", type=float, default=5)
    parser.add_argument("--validation-seconds", type=float, default=20)
    parser.add_argument("--settle-seconds", type=float, default=1.5)
    parser.add_argument("--camera-index", type=int, default=0)
    parser.add_argument("--max-cv-percent", type=float, default=1)
    parser.add_argument("--apply", action="store_true", help="Back up and replace unchanged source profile only after both modes pass validation")
    parser.add_argument("--dry-run", action="store_true", help="Print the plan without accessing hardware or writing files")
    args = parser.parse_args(argv)
    try:
        report = tune(args.profile, args.capture, args.output,
                      delays=[int(v) for v in args.delays.split(",")],
                      exposures=[float(v) for v in args.exposures.split(",")] if args.exposures else None,
                      sample_seconds=args.sample_seconds, validation_seconds=args.validation_seconds,
                      settle_seconds=args.settle_seconds, camera_index=args.camera_index,
                      policy=Policy(max_cv_percent=args.max_cv_percent), apply=args.apply, dry_run=args.dry_run)
        if args.dry_run:
            print(json.dumps(report, indent=2))
        else:
            selected = report["selected"]
            print(f"Validated: exposure {selected['exposure_us']:g} us, acquisition delay {selected['delay_us']} us. Applied: {report['applied']}. Report: {args.output.resolve() / 'REPORT.md'}")
        return 0
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print(f"Calibration failed: {error}", file=sys.stderr)
        return 1
