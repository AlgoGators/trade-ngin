#!/bin/bash

# Development Environment Setup Script
# This script installs all required tools for local development

set -e

echo "🚀 Setting up Trade Ngin development environment..."

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Function to check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to detect OS
detect_os() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "linux"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    else
        echo "unknown"
    fi
}

OS=$(detect_os)

echo -e "${BLUE}📋 Detected OS: $OS${NC}"

# Install dependencies based on OS
if [[ "$OS" == "linux" ]]; then
    echo -e "${BLUE}📦 Installing Linux dependencies...${NC}"

    # Check if apt is available (Ubuntu/Debian)
    if command_exists apt-get; then
        sudo apt-get update
        sudo apt-get install -y \
            build-essential \
            cmake \
            clang-format \
            cpplint \
            libgtest-dev \
            nlohmann-json3-dev \
            libarrow-dev \
            libpqxx-dev \
            lcov \
            gcovr
    else
        echo -e "${RED}❌ Unsupported Linux distribution. Please install dependencies manually.${NC}"
        exit 1
    fi

elif [[ "$OS" == "macos" ]]; then
    echo -e "${BLUE}📦 Installing macOS dependencies...${NC}"

    # Check if Homebrew is installed
    if ! command_exists brew; then
        echo -e "${YELLOW}⚠️  Homebrew not found. Installing Homebrew...${NC}"
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi

    # Install dependencies via Homebrew
    brew install \
        cmake \
        clang-format \
        nlohmann-json \
        apache-arrow \
        libpqxx \
        lcov

    # Install cpplint via pip
    if ! command_exists cpplint; then
        pip3 install cpplint
    fi

    # Install Google Test
    brew install googletest

else
    echo -e "${RED}❌ Unsupported operating system. Please install dependencies manually.${NC}"
    exit 1
fi

echo -e "${GREEN}✅ Dependencies installed successfully!${NC}"

# Verify installations
echo -e "${BLUE}🔍 Verifying installations...${NC}"

TOOLS=("cmake" "clang-format" "cpplint" "lcov" "gcovr")
MISSING_TOOLS=()

for tool in "${TOOLS[@]}"; do
    if command_exists "$tool"; then
        echo -e "${GREEN}✅ $tool is installed${NC}"
    else
        echo -e "${RED}❌ $tool is missing${NC}"
        MISSING_TOOLS+=("$tool")
    fi
done

if [ ${#MISSING_TOOLS[@]} -ne 0 ]; then
    echo -e "${RED}❌ Some tools are missing: ${MISSING_TOOLS[*]}${NC}"
    echo "Please install them manually and run this script again."
    exit 1
fi

# Create necessary directories
echo -e "${BLUE}📁 Creating necessary directories...${NC}"
mkdir -p build
mkdir -p linting
mkdir -p logs

# Make scripts executable
echo -e "${BLUE}🔧 Making scripts executable...${NC}"
chmod +x scripts/pre-commit-hook.sh
chmod +x linting/lint_runner.sh
chmod +x linting/auto_fix_lint.sh

# Test build
echo -e "${BLUE}🔨 Testing build process...${NC}"
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc) || echo -e "${YELLOW}⚠️  Build test failed (this is normal if dependencies are missing)${NC}"
cd ..

echo -e "${GREEN}🎉 Development environment setup complete!${NC}"
echo ""
echo -e "${BLUE}📚 Next steps:${NC}"
echo "1. Run pre-commit checks: ./scripts/pre-commit-hook.sh"
echo "2. Run linting: ./linting/lint_runner.sh"
echo "3. Build and test: cd build && cmake .. && make && ctest"
echo ""
echo -e "${BLUE}📖 For more information, see CI_CD_README.md${NC}"
