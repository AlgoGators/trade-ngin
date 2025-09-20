# Build the docker image
# run this file from the root of the project by typing ./scripts/build_docker.sh
# updated 2025-10-04

docker build -t trade-ngin .

docker run -d --name trade-ngin-dev trade-ngin
