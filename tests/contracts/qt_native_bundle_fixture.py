"""Stage only the fixed cached synthetic build; no downloads or rebuilds."""
import importlib.util
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path("/home/devcontainers/qt-validation-20260921")
BINARY = BUILD / "bin/Debug/qt_evaluator"
BUILD_ID = "local-qt-controlled"


def actual_bundle_inputs():
    assert BINARY.is_file(), "Build the actual offline evaluator first"
    inspected = subprocess.run(["/usr/bin/ldd", str(BINARY)], capture_output=True, text=True,
        timeout=15, env={"PATH":"/usr/bin:/bin", "LC_ALL":"C", "TZ":"UTC"})
    assert inspected.returncode == 0 and not inspected.stderr, "Native closure discovery failed"
    sources = {"qt_evaluator": str(BINARY)}
    for line in inspected.stdout.splitlines():
        line = line.strip()
        if re.fullmatch(r"linux-vdso\.so\.1 \(0x[0-9a-f]+\)", line): continue
        found = re.fullmatch(r"([A-Za-z0-9_.+-]+) => (/[^ ]+) \(0x[0-9a-f]+\)", line)
        if found: name, path = found.groups()
        else:
            found = re.fullmatch(r"(/[^ ]+) \(0x[0-9a-f]+\)", line)
            assert found, "Unresolved native dependency"
            path = found.group(1); name = Path(path).name
        assert name not in sources and Path(path).is_file()
        sources[name] = path
    compiler_files = list(BUILD.glob("CMakeFiles/*/CMakeCXXCompiler.cmake"))
    assert len(compiler_files) == 1
    compiler_text = compiler_files[0].read_text()
    def setting(name):
        found = re.search(r'^set\(' + re.escape(name) + r' "([^"\n]+)"\)$', compiler_text, re.M)
        assert found, "Cached compiler metadata unavailable"
        return found.group(1)
    roles = {"qt_evaluator":"executable", "libtrade_ngin.so":"engine", "ld-linux-x86-64.so.2":"loader"}
    artifacts = [{"name":name,"source":path,"role":roles.get(name,"dependency")} for name,path in sources.items()]
    metadata = {"evaluator_build":BUILD_ID,
        "compiler":{"id":setting("CMAKE_CXX_COMPILER_ID"),"version":setting("CMAKE_CXX_COMPILER_VERSION")},
        "abi":{"platform":"linux","machine":"x86_64","elf_class":"ELF64","cxx_standard":"20"}}
    return {"artifacts": artifacts, "metadata": metadata}


def stage_actual_bundle(destination):
    spec = importlib.util.spec_from_file_location("qt_offline_bundle_stager", ROOT / "apps/tools/qt_evaluator_bundle.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    inputs = actual_bundle_inputs()
    return module.stage_bundle(ROOT / "apps/tools/qt_evaluator_manifest.json",
        inputs["artifacts"], inputs["metadata"], destination)
