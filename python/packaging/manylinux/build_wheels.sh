#!/bin/bash
set -e
export PATH=/opt/python/cp312-cp312/bin:$PATH
pip install -q wheel
NL=$(find /usr/local -name 'NLoptConfig.cmake' | head -1)
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:/usr/lib64/pkgconfig
export CMAKE_ARGS="-DNLopt_DIR=$(dirname $NL)"
echo "NLopt: $NL"
rm -rf /tmp/raw /tmp/fixed; mkdir -p /tmp/raw /tmp/fixed /out; rm -rf /out/*.whl /out/*.log
for tag in ${PYTAGS:-cp312-cp312}; do
  echo "=== $tag"
  /opt/python/$tag/bin/pip wheel /src --no-deps -w /tmp/raw -v > /out/build-$tag.log 2>&1 || { tail -60 /out/build-$tag.log; exit 1; }
done
ls -la /tmp/raw
for w in /tmp/raw/*.whl; do
  auditwheel repair --plat manylinux_2_28_x86_64 -w /tmp/fixed "$w"
done
# libarrow needs libatomic.so.1, which auditwheel treats as a system library and leaves out; a
# minimal Linux install does not have it, so it travels in the wheel next to libarrow.
ATOMIC=$(find /usr/lib64 /lib64 -name 'libatomic.so.1' | head -1)
echo "libatomic: $ATOMIC"
for w in /tmp/fixed/*.whl; do
  rm -rf /tmp/unp && mkdir /tmp/unp
  python -m wheel unpack "$w" -d /tmp/unp
  d=$(echo /tmp/unp/*/)
  cp "$ATOMIC" "$d/tradengin.libs/libatomic.so.1"
  python -m wheel pack "$d" -d /out
done
ls -la /out/*.whl
