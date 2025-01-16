#!/bin/bash

# Exit on error
set -e

# Install dependencies using Homebrew (for macOS)
if [[ "$OSTYPE" == "darwin"* ]]; then
    if ! command -v brew &> /dev/null; then
        echo "Installing Homebrew..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi
    
    echo "Installing dependencies..."
    brew install cmake apache-arrow libpq libpqxx

    # Set environment variables for PostgreSQL
    export PATH="/usr/local/opt/libpq/bin:$PATH"
    export LDFLAGS="-L/usr/local/opt/libpq/lib"
    export CPPFLAGS="-I/usr/local/opt/libpq/include"
fi

# Set up API and database environment variables
export DATABENTO_API_KEY="db-7MLnkmd4uLXbsy6MshdB9jRjivG8"
export DB_HOST="3.140.200.228"
export DB_PORT="5432"
export DB_USER="postgres"
export DB_PASSWORD="algogators"
export DB_NAME="algo_data"

# Create build directory
mkdir -p build
cd build

# Configure CMake
cmake ..

# Build the project
cmake --build .

echo "Setup complete! You can now run the tests with 'ctest' in the build directory." 