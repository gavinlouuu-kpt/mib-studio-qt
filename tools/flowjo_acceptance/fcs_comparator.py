#!/usr/bin/env python3
"""Optional independent FCS comparator for the native reader (F-2).

A CI/qualification comparator only (ADR 0016): it never feeds the
production path. It re-reads an FCS file with a third-party reader
(fcsparser, else FlowKit's reader flowio) and prints one JSON object that
the native acceptance tests compare against FcsReader:

    {"reader": "fcsparser 0.2.8", "events": N, "parameter": P,
     "sum": float, "rectangle": count | null,
     "flowkit": "flowkit 1.3.2" | null, "flowkitRectangle": count | null}

usage: fcs_comparator.py FILE PARAMETER [--rect X XMIN XMAX Y YMIN YMAX] [--flowkit]

--flowkit (F-3, #808 F08/F04 comparator) also gates the rectangle with
FlowKit's own GatingML RectangleGate on its own Sample parser (raw,
uncompensated, untransformed events). GatingML bounds are min <= x < max while
MIB's interval is inclusive at both ends, so callers pick bounds no event sits
on (the FlowJo reference has no boundary events).

Raw stored values only (no $PnE/$PnG/compensation): compare it with the
native reader on float/double data or linear integer channels.
Exit 0 on success, 2 when no comparator library is installed, 1 on error.
"""
import argparse
import json
import sys


def read_columns(path):
    try:
        import fcsparser  # type: ignore
        meta, data = fcsparser.parse(path, reformat_meta=True)
        version = getattr(fcsparser, "__version__", "unknown")
        names = list(data.columns)
        return f"fcsparser {version}", {n: data[n].to_numpy(dtype=float) for n in names}
    except ImportError:
        pass
    try:
        import flowio  # type: ignore
        import numpy
        flow = flowio.FlowData(path)
        events = numpy.reshape(numpy.asarray(flow.events, dtype=float), (-1, flow.channel_count))
        if hasattr(flow, "pnn_labels"):  # flowio >= 1.3 (the FlowKit 1.3 pin)
            names = list(flow.pnn_labels)
        else:
            names = [flow.channels[str(i + 1)]["PnN"] for i in range(flow.channel_count)]
        version = getattr(flowio, "__version__", "unknown")
        return f"flowio {version}", {n: events[:, i] for i, n in enumerate(names)}
    except ImportError:
        return None, None


def flowkit_rectangle(path, rect):
    """Count events in the rectangle with FlowKit's GatingStrategy; None if absent."""
    try:
        import warnings
        warnings.filterwarnings("ignore")
        import flowkit  # type: ignore
    except ImportError:
        return None, None
    x, x0, x1, y, y0, y1 = rect
    sample = flowkit.Sample(path)
    gate = flowkit.gates.RectangleGate(
        "mib-rectangle",
        dimensions=[flowkit.Dimension(x, range_min=float(x0), range_max=float(x1)),
                    flowkit.Dimension(y, range_min=float(y0), range_max=float(y1))])
    strategy = flowkit.GatingStrategy()
    strategy.add_gate(gate, gate_path=("root",))
    result = strategy.gate_sample(sample)
    return f"flowkit {flowkit.__version__}", int(result.get_gate_count("mib-rectangle"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file")
    parser.add_argument("parameter")
    parser.add_argument("--rect", nargs=6, metavar=("X", "XMIN", "XMAX", "Y", "YMIN", "YMAX"))
    parser.add_argument("--flowkit", action="store_true",
                        help="also gate --rect with FlowKit (error if FlowKit is absent)")
    args = parser.parse_args()
    reader, columns = read_columns(args.file)
    if reader is None:
        print(json.dumps({"status": "unavailable", "error": "neither fcsparser nor flowio is installed"}))
        return 2
    try:
        values = columns[args.parameter]
        result = {"reader": reader, "events": int(len(values)), "parameter": args.parameter,
                  "sum": float(values.sum()), "rectangle": None}
        if args.rect:
            x, x0, x1, y, y0, y1 = args.rect
            xs, ys = columns[x], columns[y]
            inside = (xs >= float(x0)) & (xs <= float(x1)) & (ys >= float(y0)) & (ys <= float(y1))
            result["rectangle"] = int(inside.sum())
        result["flowkit"] = None
        result["flowkitRectangle"] = None
        if args.flowkit:
            if not args.rect:
                raise ValueError("--flowkit needs --rect")
            name, count = flowkit_rectangle(args.file, args.rect)
            if name is None:
                print(json.dumps({"status": "unavailable", "error": "flowkit is not installed"}))
                return 2
            result["flowkit"], result["flowkitRectangle"] = name, count
    except (KeyError, ValueError) as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
