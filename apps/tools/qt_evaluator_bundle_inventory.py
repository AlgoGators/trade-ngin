"""Inspect one built evaluator and emit the explicit bundle dependency inventory."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


_MAPPED = re.compile(r"([A-Za-z0-9_.+-]+) => (/[^ ]+) \(0x[0-9A-Fa-f]+\)\Z")
_DIRECT = re.compile(r"(/[^ ]+) \(0x[0-9A-Fa-f]+\)\Z")
_VDSO = re.compile(r"linux-vdso\.so\.1 \(0x[0-9A-Fa-f]+\)\Z")


def canonical(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True, allow_nan=False,
                      separators=(",", ":")).encode("ascii")


def parse_ldd(text):
    if not isinstance(text, str):
        raise ValueError("invalid_ldd_output")
    result = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if _VDSO.fullmatch(line):
            continue
        mapped = _MAPPED.fullmatch(line)
        if mapped:
            name, source = mapped.groups()
        else:
            direct = _DIRECT.fullmatch(line)
            if not direct:
                raise ValueError("unresolved_native_dependency")
            source = direct.group(1)
            name = Path(source).name
        path = Path(source)
        if name in result or not path.is_absolute() or Path(name).name != name:
            raise ValueError("ambiguous_native_dependency")
        result[name] = path
    if not result:
        raise ValueError("empty_native_dependency_inventory")
    return result


def inventory(evaluator, engine, build_id, compiler_id, compiler_version, dependencies, *, live_config=False):
    executable_name="live_config_validate" if live_config else "qt_evaluator"
    evaluator = Path(evaluator).resolve(strict=True)
    engine = Path(engine).resolve(strict=True)
    if (not evaluator.is_file() or not engine.is_file() or evaluator.name != executable_name or
            engine.name != "libtrade_ngin.so" or
            not isinstance(dependencies, dict) or
            dependencies.get("libtrade_ngin.so") is None or
            Path(dependencies["libtrade_ngin.so"]).resolve(strict=True) != engine):
        raise ValueError("bundle_build_target_mismatch")
    rows = [{"name": executable_name, "source": str(evaluator), "role": "executable"}]
    for name, source in dependencies.items():
        source = Path(source)
        if (Path(name).name != name or not source.is_absolute() or
                source.name != name or not source.is_file()):
            raise ValueError("invalid_native_dependency")
        source = source.resolve(strict=True)
        if not source.is_file():
            raise ValueError("invalid_native_dependency")
        role = ("engine" if name == "libtrade_ngin.so" else
                "loader" if name == "ld-linux-x86-64.so.2" else "dependency")
        rows.append({"name": name, "source": str(source), "role": role})
    names = [row["name"] for row in rows]
    if (len(names) != len(set(names)) or "libcrypto.so.3" not in names or
            "ld-linux-x86-64.so.2" not in names):
        raise ValueError("incomplete_native_dependency_inventory")
    rows.sort(key=lambda row: row["name"])
    return {
        "artifacts": rows,
        "metadata": {
            ("validator_build" if live_config else "evaluator_build"): build_id,
            "compiler": {"id": compiler_id, "version": compiler_version},
            "abi": {"platform": "linux", "machine": "x86_64", "elf_class": "ELF64",
                    "cxx_standard": "20"},
        },
    }


def inspect(evaluator, engine, build_id, compiler_id, compiler_version, *, live_config=False):
    evaluator = Path(evaluator).resolve(strict=True)
    engine = Path(engine).resolve(strict=True)
    environment = {
        "PATH": "/usr/bin:/bin",
        "LC_ALL": "C",
        "TZ": "UTC",
        "LD_LIBRARY_PATH": str(engine.parent),
    }
    completed = subprocess.run(["/usr/bin/ldd", str(evaluator)], capture_output=True,
                               text=True, timeout=30, env=environment)
    if completed.returncode != 0 or completed.stderr:
        raise ValueError("native_dependency_inspection_failed")
    return inventory(evaluator, engine, build_id, compiler_id, compiler_version,
                     parse_ldd(completed.stdout), live_config=live_config)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evaluator", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--build-id", required=True)
    parser.add_argument("--compiler-id", required=True)
    parser.add_argument("--compiler-version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--live-config", action="store_true")
    args = parser.parse_args()
    if args.output.exists() or args.output.is_symlink():
        raise ValueError("bundle_inventory_exists")
    value = inspect(args.evaluator, args.engine, args.build_id,
                    args.compiler_id, args.compiler_version, live_config=args.live_config)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("xb") as stream:
        stream.write(canonical(value) + b"\n")
    print(args.output)


if __name__ == "__main__":
    main()
