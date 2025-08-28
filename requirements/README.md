Trade Ngin - Requirements

This folder contains ready-to-run scripts to install system dependencies for building and running the project.

Supported OS:
- Ubuntu 22.04+ (Debian-based)
- macOS (Apple Silicon and Intel via Homebrew)

Usage
1) Ubuntu
```bash
sudo bash requirements/install_ubuntu.sh
```

2) macOS (Homebrew required)
```bash
bash requirements/install_macos.sh
```

Installed Dependencies
- Build tools: cmake, make, gcc/clang, pkg-config, git
- Testing: GoogleTest (GTest)
- Libraries:
  - nlohmann_json
  - Apache Arrow (libarrow, dataset)
  - PostgreSQL client libs (libpq)
  - libpqxx (C++ client for Postgres)

Notes
- On Ubuntu we add the Apache Arrow APT source and install Arrow dev packages.
- On macOS we use Homebrew for all packages; ensure `/opt/homebrew` is on your PATH for Apple Silicon.
- If libpqxx via Homebrew is not discovered automatically, the project uses pkg-config (`libpqxx.pc`).


