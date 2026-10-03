#!/usr/bin/env python3

import argparse
import shutil
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--print-inputs", action="store_true")
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--stamp", type=Path)
    args = parser.parse_args()

    if not (args.source / "devtools_app.html").is_file():
        raise RuntimeError(f"DevTools frontend is incomplete: {args.source}")
    if args.print_inputs:
        for path in sorted(args.source.rglob("*")):
            if path.is_file():
                print(path.resolve().as_posix())
        return 0
    if args.destination is None or args.stamp is None:
        parser.error("--destination and --stamp are required when copying")

    if args.destination.exists():
        shutil.rmtree(args.destination)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(args.source, args.destination)
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text("staged\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
