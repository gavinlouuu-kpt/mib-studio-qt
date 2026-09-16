"""Sequential hardware sweep, independent validation, and guarded profile apply."""

from dataclasses import asdict
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile

from .analysis import Policy, analyze_capture, select_candidate

MODES = ("overview", "experiment")


def write_json(path, document):
    Path(path).write_text(json.dumps(document, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def candidate_profile(original, exposure, delay):
    result = dict(original)
    result["exposure_time_us"] = exposure
    result["acq_trigger_delay_us"] = delay
    return result


def make_plan(original, delays, exposures):
    """Pure preflight; the native parser performs full profile validation too."""
    if (not isinstance(original, dict) or not isinstance(original.get("live_view"), dict)
            or original["live_view"].get("enabled") is not True):
        raise ValueError("An enabled illuminated Live View profile is required")
    delays, exposures = list(delays), list(exposures)
    if not delays or not exposures or len(delays) * len(exposures) > 128:
        raise ValueError("Supply 1–128 exposure/delay combinations")
    if any(type(d) is not int or not 0 <= d <= 1000000 for d in delays):
        raise ValueError("Delays must be integer microseconds between 0 and 1000000")
    if any(type(e) not in (float, int) or not math.isfinite(e) or not .8 <= e <= 838860 for e in exposures):
        raise ValueError("Exposures must be finite, within the XGC range 0.8–838860 us")
    if len(set(delays)) != len(delays) or len(set(exposures)) != len(exposures):
        raise ValueError("Duplicate sweep points")
    frequency = original["live_view"]["frequency_hz"]
    if type(frequency) not in (float, int) or not math.isfinite(frequency) or not 400 <= frequency <= 40000:
        raise ValueError("Invalid trigger frequency")
    period = 1e6 / frequency
    profiles = []
    for exposure, delay in itertools.product(sorted(exposures), sorted(delays)):
        if delay + exposure >= period:
            raise ValueError(f"Exposure plus acquisition delay must fit the trigger period: {exposure} + {delay} >= {period}")
        profiles.append(candidate_profile(original, exposure, delay))
    return profiles


class NativeSampler:
    def __init__(self, executable: Path, camera_index=0):
        self.executable = Path(executable).resolve()
        self.camera_index = camera_index
        if not self.executable.is_file():
            raise ValueError(f"Build mib_sync_capture first: {self.executable}")

    def validate(self, profile: Path):
        result = subprocess.run([str(self.executable), "--profile", str(profile), "--validate"],
                                capture_output=True, timeout=15)
        if result.returncode:
            raise ValueError(f"Native profile validation failed: {profile}\n" + result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace"))

    def capture(self, profile: Path, mode: str, directory: Path, seconds: float, settle: float):
        command = [str(self.executable), "--profile", str(profile), "--output", str(directory),
                   "--mode", mode, "--seconds", str(seconds), "--settle", str(settle),
                   "--camera-index", str(self.camera_index)]
        # No shell, inherited console input, focus control, or process termination
        # of an existing app. A cancel file lets the native owner gate the LED off.
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        with directory.with_suffix(".log").open("wb") as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, creationflags=flags)
            try:
                code = process.wait(timeout=seconds + settle + 60)
            except (KeyboardInterrupt, subprocess.TimeoutExpired) as error:
                if directory.exists(): (directory / "cancel.request").touch()
                try:
                    process.wait(timeout=35)
                except (subprocess.TimeoutExpired, KeyboardInterrupt):
                    process.kill()
                    process.wait(timeout=10)
                    raise RuntimeError("Capture did not shut down cooperatively. LED/generator OFF is unconfirmed; inspect the rig. No settings were applied.") from error
                raise RuntimeError("Calibration cancelled/timed out; inspect capture.json for shutdown confirmation. No settings were applied.") from error
        if code:
            raise RuntimeError(f"Capture failed ({code}); sweep stopped. See {directory.with_suffix('.log')} and capture.json for shutdown status")


def apply_profile(profile: Path, original_bytes: bytes, recommended: dict, backup: Path):
    """Back up exact original bytes and atomically replace only an unchanged file."""
    profile, backup = Path(profile), Path(backup)
    if profile.read_bytes() != original_bytes:
        raise RuntimeError("Profile changed during calibration; refusing to overwrite it")
    if backup.exists():
        if backup.read_bytes() != original_bytes:
            raise RuntimeError("Backup does not match the original profile")
    else:
        with backup.open("xb") as stream:
            stream.write(original_bytes)
            stream.flush()
            os.fsync(stream.fileno())
    payload = (json.dumps(recommended, indent=2, allow_nan=False) + "\n").encode("utf-8")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="wb", prefix=profile.name + ".", suffix=".tmp",
                                         dir=profile.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        if profile.read_bytes() != original_bytes:
            raise RuntimeError("Profile changed while preparing the update; refusing overwrite")
        os.replace(temporary, profile)
        temporary = None
        if profile.read_bytes() != payload:
            raise RuntimeError("Saved profile verification failed")
    finally:
        if temporary is not None: temporary.unlink(missing_ok=True)


def tune(profile, capture_executable=None, output=None, *,
         delays=(0, 5, 15, 30, 45, 60, 75), exposures=None,
         sample_seconds=5.0, validation_seconds=20.0, settle_seconds=1.5,
         camera_index=0, policy=Policy(), apply=False, dry_run=False,
         sampler=None, progress=print):
    """Measure both modes, sweep delays/exposures, validate, optionally apply.

    Close camera-owning applications first. `apply=False` leaves the source
    profile untouched. Supply a new output directory for every invocation.
    A sampler with validate/capture methods may be injected by an integration.
    """
    profile = Path(profile).resolve()
    original_bytes = profile.read_bytes()
    original = json.loads(original_bytes)
    exposures = tuple(exposures) if exposures is not None else (original["exposure_time_us"],)
    planned = make_plan(original, delays, exposures)
    if not (isinstance(camera_index, int) and 0 <= camera_index <= 1024):
        raise ValueError("Invalid camera index")
    for duration in (sample_seconds, validation_seconds):
        if not math.isfinite(duration) or not 1 <= duration <= 120:
            raise ValueError("Measurement durations must be 1–120 seconds")
    if validation_seconds < sample_seconds or not math.isfinite(settle_seconds) or not 0 <= settle_seconds <= 10:
        raise ValueError("Validation must be at least as long as the sweep; settling must be 0–10 seconds")
    if dry_run and apply: raise ValueError("--apply cannot be combined with --dry-run")
    plan = dict(schema_version=1, profile=str(profile), source_sha256=hashlib.sha256(original_bytes).hexdigest(),
                policy=asdict(policy), sample_seconds=sample_seconds, validation_seconds=validation_seconds,
                settle_seconds=settle_seconds, modes=list(MODES), camera_index=camera_index,
                candidates=[dict(exposure_us=c["exposure_time_us"], delay_us=c["acq_trigger_delay_us"]) for c in planned])
    if dry_run:
        return dict(plan, status="dry_run", note="No hardware accessed or files changed. Native profile validation occurs before a real sweep.")
    if output is None: raise ValueError("An output directory is required")
    if sampler is None:
        if capture_executable is None: raise ValueError("A capture executable is required")
        sampler = NativeSampler(capture_executable, camera_index)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    backup = output / "original.json"
    backup.write_bytes(original_bytes)
    write_json(output / "plan.json", plan)
    report = dict(plan, status="running", applied=False, candidates=[])
    def save(): write_json(output / "report.json", report)
    def measure(path, prefix, seconds, baseline=None):
        modes = {}
        for mode in MODES:
            progress(f"{prefix}: {mode}, {seconds:g}s + {settle_seconds:g}s settling")
            directory = output / f"{prefix}-{mode}"
            sampler.capture(path, mode, directory, seconds, settle_seconds)
            metadata = json.loads((directory / "capture.json").read_text(encoding="utf-8"))
            if (metadata.get("mode") != mode or metadata.get("profile") != json.loads(path.read_bytes())
                    or metadata.get("seconds") != seconds):
                raise ValueError("Sampler evidence does not match requested mode/profile/duration")
            modes[mode] = analyze_capture(directory, policy)
            if baseline and modes[mode]["capture_fps"] < baseline[mode]["capture_fps"] * policy.min_rate_ratio:
                modes[mode]["accepted"] = False
                modes[mode]["reasons"].append("capture rate fell below baseline tolerance")
        return modes
    try:
        # Validate every proposed profile before enabling any hardware output.
        sampler.validate(backup)
        paths = []
        for i, config in enumerate(planned):
            path = output / f"candidate-{i:03d}.json"
            write_json(path, config)
            sampler.validate(path)
            paths.append(path)
        report["baseline"] = measure(backup, "baseline", sample_seconds)
        save()
        for i, (config, path) in enumerate(zip(planned, paths)):
            modes = measure(path, f"candidate-{i:03d}", sample_seconds, report["baseline"])
            report["candidates"].append(dict(exposure_us=config["exposure_time_us"],
                delay_us=config["acq_trigger_delay_us"], profile=str(path), modes=modes))
            save()
        chosen = select_candidate(report["candidates"], policy)
        recommendation = candidate_profile(original, chosen["exposure_us"], chosen["delay_us"])
        report["selected"] = chosen
        validation_profile = output / "validation-profile.json"
        write_json(validation_profile, recommendation)
        validation = measure(validation_profile, "validation", validation_seconds, report["baseline"])
        report["validation"] = validation
        for mode in MODES:
            allowed = max(chosen["modes"][mode]["score"] * 2, chosen["modes"][mode]["score"] + policy.near_best_margin_percent)
            if validation[mode]["score"] > allowed:
                validation[mode]["accepted"] = False
                validation[mode]["reasons"].append("long validation is less stable than the sweep")
            baseline = report["baseline"][mode]
            if baseline["accepted"] and validation[mode]["score"] > max(baseline["score"] * 1.25, baseline["score"] + policy.near_best_margin_percent):
                validation[mode]["accepted"] = False
                validation[mode]["reasons"].append("worse than already acceptable baseline")
        if not all(m["accepted"] for m in validation.values()):
            raise ValueError("Long validation failed; no recommendation applied (see report.json)")
        write_json(output / "recommended.json", recommendation)
        report["status"] = "validated"
        save()
        from .report import write_report
        write_report(output, report)
        if apply:
            apply_profile(profile, original_bytes, recommendation, backup)
            report["applied"] = True
            save()
            # Plot/report generation has already succeeded before profile mutation.
            summary = output / "REPORT.md"
            summary.write_text(summary.read_text(encoding="utf-8").replace(
                "Applied to source profile: **False**", "Applied to source profile: **True**"), encoding="utf-8")
        return report
    except BaseException as error:
        report["status"] = "failed"
        report["error"] = str(error)
        save()
        try:
            from .report import write_report
            write_report(output, report)
        except Exception:
            pass  # Preserve the original failure; report.json remains authoritative.
        raise
