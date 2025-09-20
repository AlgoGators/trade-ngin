#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Please run as root: sudo $0" >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

echo "[1/6] Updating apt index..."
apt-get update

echo "[2/6] Installing base build tools and essentials..."
apt-get install -y \
  build-essential \
  cmake \
  git \
  wget \
  gnupg \
  lsb-release \
  pkg-config \
  libpq-dev

echo "[3/6] Installing GoogleTest from source..."
apt-get install -y libgtest-dev
tmpdir=$(mktemp -d)
git clone https://github.com/google/googletest.git "${tmpdir}/googletest"
cd "${tmpdir}/googletest"
mkdir -p build && cd build
cmake ..
make -j"$(nproc)"
make install

echo "[4/6] Adding Apache Arrow APT repository..."
wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add -
codename=$(lsb_release -cs)
wget -O /tmp/apache-arrow-apt-source.deb "https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-${codename}.deb" || true
apt-get update || true
apt-get install -y /tmp/apache-arrow-apt-source.deb || true
apt-get update

echo "[5/6] Installing Apache Arrow development packages..."
apt-get install -y \
  libarrow-dev \
  libarrow-dataset-dev

echo "[6/6] Installing nlohmann_json and libpqxx from source (with PIC)..."
cd "${tmpdir}"
git clone https://github.com/nlohmann/json.git
cd json && mkdir -p build && cd build
cmake .. -DCMAKE_POSITION_INDEPENDENT_CODE=ON
make -j"$(nproc)" && make install

cd "${tmpdir}"
git clone https://github.com/jtv/libpqxx.git
cd libpqxx && mkdir -p build && cd build
cmake .. -DSKIP_BUILD_TEST=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=ON
make -j"$(nproc)" && make install

ldconfig

echo "All dependencies installed successfully."
