#!/usr/bin/env bash
set -euo pipefail

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew not found. Install from https://brew.sh and re-run." >&2
  exit 1
fi

echo "[1/5] Updating Homebrew..."
brew update

echo "[2/5] Installing toolchain and essentials..."
brew install \
  cmake \
  pkg-config \
  git \
  jq

echo "[3/5] Installing libraries (Arrow, Postgres, libpqxx, JSON, GTest)..."
brew install \
  apache-arrow \
  nlohmann-json \
  libpq \
  libpqxx \
  googletest

echo "[4/5] Ensuring libpq tools are linked into PATH (optional)..."
# On macOS, Homebrew may not link libpq binaries by default
if ! command -v psql >/dev/null 2>&1; then
  brew link --force libpq || true
fi

echo "[5/5] Exporting common pkg-config path for Apple Silicon (zsh/bash)"
HOMEBREW_PREFIX=$(brew --prefix)
if [[ ":${PKG_CONFIG_PATH:-}:" != *":${HOMEBREW_PREFIX}/lib/pkgconfig:"* ]]; then
  echo "Consider adding to your shell profile:"
  echo "export PKG_CONFIG_PATH=\"${HOMEBREW_PREFIX}/lib/pkgconfig:${PKG_CONFIG_PATH:-}\""
fi

echo "All dependencies installed successfully via Homebrew."
