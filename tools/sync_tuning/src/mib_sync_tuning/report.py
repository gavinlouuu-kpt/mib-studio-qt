"""Portable Markdown/CSV evidence with optional matplotlib plot."""

import csv
from pathlib import Path


def write_report(output: Path, report: dict):
    rows = []
    groups = [("baseline", report.get("baseline", {}))]
    groups += [(f"candidate-{i:03d}", c["modes"]) for i, c in enumerate(report.get("candidates", []))]
    groups += [("validation", report.get("validation", {}))]
    for label, modes in groups:
        for mode, metrics in modes.items():
            rows.append(dict(run=label, mode=mode, accepted=metrics["accepted"],
                             reasons="; ".join(metrics["reasons"]), **{key: metrics[key] for key in (
                "frames", "mean", "cv_percent", "score", "max_clipped_fraction", "reader_missing", "capture_fps")}))
    if rows:
        with (output / "summary.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0])
            writer.writeheader()
            writer.writerows(rows)
    selected = report.get("selected")
    text = ["# MindVision / LED calibration", "", f"Status: **{report['status']}**.",
            f"Applied to source profile: **{report['applied']}**. Original bytes: `original.json`."]
    if selected:
        text += [f"Selected exposure: **{selected['exposure_us']:g} µs**; acquisition delay: **{selected['delay_us']} µs**.",
                 f"Tested stable delay interval: {selected['plateau_delays_us']} µs."]
    if report.get("error"): text += ["", "Failure: " + report["error"]]
    text += ["",
            "| Run | Mode | Frames | Mean /255 | Temporal CV | Worst band/mean CV | FPS | Reader misses |",
            "|---|---|---:|---:|---:|---:|---:|---:|"]
    for row in rows:
        text.append(f"| {row['run']} | {row['mode']} | {row['frames']} | {row['mean']:.2f} | {row['cv_percent']:.3f}% | {row['score']:.3f}% | {row['capture_fps']:.1f} | {row['reader_missing']} |")
    rejected = [f"- {r['run']} / {r['mode']}: {r['reasons']}" for r in rows if not r["accepted"]]
    if rejected: text += ["", "Rejected measurements:", "", *rejected]
    text += ["", "CV is temporal standard deviation divided by mean. Selection also checks three horizontal bands, per-frame clipping/darkness, measured duration, reader misses and capture rate. See `report.json` for rejection reasons, telemetry validity and policy thresholds.",
             "", "Images are raw MONO8, sampled every fourth column and second row in the central half-height of the saved experiment ROI. Overview samples the same sensor coordinates. Startup settling frames are excluded. No focus/pump/sorting commands are sent. Image-based stability is not a measurement of physical LED current or sensor exposure edges.",
             "", "Reopen the app to load applied settings and recapture its background before processing. This tool does not start/stop desktop applications."]
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        text += ["", "Install the `plot` extra to generate a PNG plot; CSV evidence is complete without it."]
    else:
        if "baseline" not in report or "validation" not in report:
            (output / "REPORT.md").write_text("\n".join(text) + "\n", encoding="utf-8")
            return
        fig, axes = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
        for ax, mode in zip(axes, ("experiment", "overview")):
            for label, color in (("baseline", "#bd423c"), ("validation", "#087f8c")):
                metrics = report[label][mode]
                with (Path(metrics["directory"]) / "frames.csv").open(newline="", encoding="utf-8") as stream:
                    samples = list(csv.DictReader(stream))
                start = int(samples[0]["host_us"])
                ax.plot([(int(s["host_us"]) - start) / 1e6 for s in samples],
                        [100 * (float(s["mean"]) / metrics["mean"] - 1) for s in samples],
                        color=color, linewidth=.5, label=label)
            ax.set_title(mode.capitalize())
            ax.set_ylabel("Mean deviation (%)")
            ax.grid(alpha=.2)
            ax.legend()
        axes[-1].set_xlabel("Seconds after settling")
        fig.tight_layout()
        fig.savefig(output / "brightness-stability.png", dpi=160)
        plt.close(fig)
        text += ["", "![Brightness stability](brightness-stability.png)"]
    (output / "REPORT.md").write_text("\n".join(text) + "\n", encoding="utf-8")
