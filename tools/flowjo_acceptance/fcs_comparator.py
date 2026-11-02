#!/usr/bin/env python3
"""Optional independent FCS comparator for the native reader (F-2).

A CI/qualification comparator only (ADR 0016): it never feeds the
production path. It re-reads an FCS file with a third-party reader
(fcsparser, else FlowKit's reader flowio) and prints one JSON object that
the native acceptance tests compare against FcsReader:

    {"reader": "fcsparser 0.2.8", "events": N, "parameter": P,
     "sum": float, "rectangle": count | null}

usage: fcs_comparator.py FILE PARAMETER [--rect X XMIN XMAX Y YMIN YMAX]

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
        names = [flow.channels[str(i + 1)]["PnN"] for i in range(flow.channel_count)]
        version = getattr(flowio, "__version__", "unknown")
        return f"flowio {version}", {n: events[:, i] for i, n in enumerate(names)}
    except ImportError:
        return None, None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file")
    parser.add_argument("parameter")
    parser.add_argument("--rect", nargs=6, metavar=("X", "XMIN", "XMAX", "Y", "YMIN", "YMAX"))
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
    except (KeyError, ValueError) as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}))
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
