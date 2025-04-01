#!/bin/bash
# docker-run.sh - Script to simplify using Docker for development

# Default command
COMMAND="up"

# Display help
function show_help() {
    echo "Usage: $0 [command] [options]"
    echo "Run Docker operations for Trade-Ngin development."
    echo ""
    echo "Commands:"
    echo "  up                 Start containers (default)"
    echo "  down               Stop and remove containers"
    echo "  build              Build containers"
    echo "  shell              Open a shell in the dev container"
    echo "  exec [command]     Execute a command in the dev container"
    echo "  build-project      Build the project inside the dev container"
    echo "  run-tests          Run tests inside the dev container"
    echo "  clean              Clean all containers and volumes"
    echo ""
    echo "Examples:"
    echo "  $0                 Start all containers"
    echo "  $0 shell           Open a shell in the dev container"
    echo "  $0 exec 'ls -la'   Run 'ls -la' in the dev container"
    echo "  $0 build-project   Build the project in the dev container"
}

# Parse command
if [ $# -gt 0 ]; then
    case "$1" in
        up|down|build|shell|exec|build-project|run-tests|clean)
            COMMAND="$1"
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown command: $1"
            show_help
            exit 1
            ;;
    esac
fi

# Execute command
case "$COMMAND" in
    up)
        echo "Starting containers..."
        docker-compose up -d
        echo "Containers started. Access pgAdmin at http://localhost:5050"
        ;;
    down)
        echo "Stopping containers..."
        docker-compose down
        ;;
    build)
        echo "Building containers..."
        docker-compose build
        ;;
    shell)
        echo "Opening shell in dev container..."
        docker-compose exec dev bash
        ;;
    exec)
        if [ $# -eq 0 ]; then
            echo "Error: No command specified for exec"
            show_help
            exit 1
        fi
        echo "Executing command in dev container: $@"
        docker-compose exec dev bash -c "$@"
        ;;
    build-project)
        echo "Building project in dev container..."
        docker-compose exec dev ./build.sh
        ;;
    run-tests)
        echo "Running tests in dev container..."
        docker-compose exec dev ./build.sh --test
        ;;
    clean)
        echo "Cleaning all containers and volumes..."
        docker-compose down -v
        ;;
    *)
        echo "Unknown command: $COMMAND"
        show_help
        exit 1
        ;;
esac

exit 0 