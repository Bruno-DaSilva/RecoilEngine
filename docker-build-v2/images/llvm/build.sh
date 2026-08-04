#!/bin/bash
cd "$(dirname "$(readlink -f "$0")")/.."
source ../_resolve_container_runtime.sh
exec ${RUNTIME} build \
    -t recoil-build-llvm \
    --platform=linux/amd64 \
    -f llvm/Dockerfile "$@" .
