#!/bin/sh
# Configure Mesa (research/mesa, + tools/sweetgl/mesa.patch) for iOS 16 arm64:
# gallium freedreno + GLES, no window system, no LLVM -> research/mesa-ios.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode_15.4.app/Contents/Developer}
export PATH=$R/research/tools/bin:$R/research/venv/bin:${PATH}
python3 "$R/tools/sweetgl/gen-cross.py"
cd "$R/research/mesa"
rm -rf ../mesa-ios
meson setup ../mesa-ios --cross-file "$R/tools/sweetgl/ios-cross.txt" \
	--buildtype=release -Db_ndebug=true -Ddefault_library=static \
	-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
	-Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=msm \
	-Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
	-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
	-Dgallium-va=disabled -Dgallium-rusticl=false -Dtools= \
	-Dhtml-docs=disabled -Ddisplay-info=disabled -Dteflon=false "$@"
