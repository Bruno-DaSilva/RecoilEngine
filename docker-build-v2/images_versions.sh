#!/bin/bash
declare -A image_version
image_version[amd64-linux]=sha256:31b3667ba0d37620b4ad9b45d1840c8d3e95317f2b233044d2e17697ce8fe61d
image_version[arm64-linux]=sha256:82e7f380adc4e2a5be0d63a27eef0095302f6a3d1843ee23b60ebc05f4f9fe66
image_version[amd64-windows]=sha256:3ba630ac0c181a95dde522c3a4a81df2302914c7c4e6674e6bde0d4f6bf058ef
# The llvm platforms share the recoil-build-llvm image, which cross-compiles
# every target. Digests empty until the image CI publishes it; build.sh
# explains how to build it locally.
image_version[amd64-windows-llvm]=
image_version[amd64-linux-llvm]=
image_version[arm64-linux-llvm]=
declare -A image_name image_dir
for p in amd64-windows-llvm amd64-linux-llvm arm64-linux-llvm; do
  image_name[$p]=recoil-build-llvm
  image_dir[$p]=llvm
done
