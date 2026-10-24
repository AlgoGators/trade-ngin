# Building and publishing the `tradengin` wheels

`.github/workflows/python-wheels.yml` builds the wheels, tests each in a clean environment and, on
a version tag, uploads them to PyPI.

| | Linux | Windows |
|---|---|---|
| Wheel | `manylinux_2_28_x86_64` | `win_amd64` |
| Built in | `manylinux/Dockerfile` (Arrow, libpqxx, NLopt, Eigen 3.4 built in) | `windows-latest`, MSVC, vcpkg (`windows/vcpkg-packages.txt`) |
| Libraries bundled by | `auditwheel` (plus `libatomic.so.1`, which it leaves out) | `delvewheel` |
| Python | 3.10, 3.11, 3.12, 3.13 | 3.10, 3.11, 3.12, 3.13 |

Every wheel is installed into a clean interpreter (a `python:3.x-slim` container on Linux, a fresh
`setup-python` on Windows) and runs `examples/yfinance_backtest.py --synthetic`, which must give the
same return and trade count on every platform.

## Releasing

1. Change `version` in `pyproject.toml` (PyPI takes a file name once, so a changed wheel needs a
   new version) and merge.
2. Push a tag `vX.Y.Z` that matches it. The workflow builds, tests and uploads; it refuses a tag
   that does not match `pyproject.toml`.

The upload skips files PyPI already holds, so a failed run can be re-run, and a platform added later
joins an existing version. No sdist is published: the pybind11 submodule is not in one.

## Credentials

The upload uses the `PYPI_API_TOKEN` repository secret (a token scoped to the `tradengin` project).
PyPI "trusted publishing" would remove the stored token: add this repository and workflow under the
project's Publishing settings on pypi.org, then drop `password:` from the publish step.

## Building by hand

Linux (Docker): `docker build -t tn-wheel python/packaging/manylinux` then
`docker run --rm -v "$PWD":/src -v "$PWD/out":/out -e PYTAGS="cp312-cp312" tn-wheel bash /src/python/packaging/manylinux/build_wheels.sh`.

Windows: install the vcpkg packages in `windows/vcpkg-packages.txt` (triplet `x64-windows`), then
`pip wheel . --no-deps` with `CMAKE_ARGS=-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
-DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF`, and `delvewheel repair` the result.
