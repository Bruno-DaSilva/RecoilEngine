#!/bin/bash
# All platforms are built by the recoil-build-llvm image, which cross-compiles
# every target from amd64. The per-platform digests below all point at it and
# are stamped by the docker-images-build workflow on publish.
declare -A image_version image_name image_dir
for p in amd64-windows amd64-linux arm64-linux; do
  image_name[$p]=recoil-build-llvm
  image_dir[$p]=llvm
done
image_version[amd64-windows]=
image_version[amd64-linux]=
image_version[arm64-linux]=
