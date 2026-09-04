#!/usr/bin/env bash

set -euo pipefail

target="${MACOSX_DEPLOYMENT_TARGET:-12.0}"
work_root="${RUNNER_TEMP:-/tmp}/mpv-macos-compatible"
source_root="${work_root}/src"
dependency_prefix="${work_root}/prefix"

rm -rf "${work_root}" build
mkdir -p "${source_root}" "${dependency_prefix}"

export CFLAGS="-O2 -arch x86_64 -mmacosx-version-min=${target}"
export CXXFLAGS="${CFLAGS}"
export OBJCFLAGS="${CFLAGS}"
export LDFLAGS="-arch x86_64 -mmacosx-version-min=${target}"
export PKG_CONFIG_PATH="${dependency_prefix}/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="${dependency_prefix}/lib/pkgconfig:/usr/lib/pkgconfig"
export PATH="${dependency_prefix}/bin:${PATH}"

checkout_source() {
    local name="$1"
    local url="$2"
    local revision="$3"
    local destination="${source_root}/${name}"

    git init -q "${destination}"
    git -C "${destination}" remote add origin "${url}"
    git -C "${destination}" fetch -q --depth 1 origin "${revision}"
    git -C "${destination}" checkout -q --detach FETCH_HEAD
}

meson_static_install() {
    local name="$1"
    shift
    local build_dir="${work_root}/build-${name}"

    meson setup "${build_dir}" "${source_root}/${name}" \
        --prefix="${dependency_prefix}" \
        --buildtype=release \
        --default-library=static \
        -Db_lto=false \
        "$@"
    meson compile -C "${build_dir}" -j4
    meson install -C "${build_dir}"
}

# These revisions are the released sources used by this bundle. Building them
# here avoids importing Homebrew bottles whose current Intel builds require
# macOS 14 or 15 even when mpv itself is marked for an older deployment target.
checkout_source freetype \
    https://gitlab.freedesktop.org/freetype/freetype.git \
    42608f77f20749dd6ddc9e0536788eaad70ea4b5
checkout_source fribidi \
    https://github.com/fribidi/fribidi.git \
    68162babff4f39c4e2dc164a5e825af93bda9983
checkout_source harfbuzz \
    https://github.com/harfbuzz/harfbuzz.git \
    3ef8709829a5884517ad91a97b32b9435b2f20d1
checkout_source libass \
    https://github.com/libass/libass.git \
    4a05d8127f525943ebf45fdc6497c9e665947f0d
checkout_source libplacebo \
    https://code.videolan.org/videolan/libplacebo.git \
    cee9b076f2c63104ccfd497fa79c39a867293ec4
checkout_source luajit \
    https://github.com/LuaJIT/LuaJIT.git \
    c6ffc141a8762b41703f9287d63d93622a13dd8f

git -C "${source_root}/libplacebo" submodule update -q --init --depth 1 --recursive

meson_static_install freetype \
    -Dbrotli=disabled \
    -Dbzip2=disabled \
    -Dharfbuzz=disabled \
    -Dpng=disabled \
    -Dtests=disabled \
    -Dzlib=system

meson_static_install fribidi \
    -Dbin=false \
    -Ddeprecated=false \
    -Ddocs=false \
    -Dtests=false

meson_static_install harfbuzz \
    -Dfreetype=enabled \
    -Dcoretext=enabled \
    -Dglib=disabled \
    -Dgobject=disabled \
    -Dcairo=disabled \
    -Dchafa=disabled \
    -Dicu=disabled \
    -Dtests=disabled \
    -Dintrospection=disabled \
    -Ddocs=disabled \
    -Dutilities=disabled

meson_static_install libass \
    -Dcoretext=enabled \
    -Dfontconfig=disabled \
    -Ddirectwrite=disabled \
    -Dlibunibreak=disabled \
    -Dtest=disabled \
    -Dcompare=disabled \
    -Dprofile=disabled \
    -Dfuzz=disabled \
    -Dcheckasm=disabled

meson_static_install libplacebo \
    -Dvulkan=disabled \
    -Dopengl=enabled \
    -Dd3d11=disabled \
    -Dglslang=disabled \
    -Dshaderc=disabled \
    -Dlcms=disabled \
    -Ddovi=disabled \
    -Dlibdovi=disabled \
    -Dxxhash=disabled \
    -Ddemos=false \
    -Dtests=false \
    -Dbench=false \
    -Dfuzz=false

make -C "${source_root}/luajit" -j4 amalg \
    PREFIX="${dependency_prefix}" \
    BUILDMODE=static \
    CFLAGS="${CFLAGS}"
make -C "${source_root}/luajit" install PREFIX="${dependency_prefix}"

ffmpeg_archive="${work_root}/ffmpeg-${FFMPEG_VERSION}.tar.xz"
curl -fL --retry 3 --retry-delay 2 \
    "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
    -o "${ffmpeg_archive}"
echo "${FFMPEG_SHA256}  ${ffmpeg_archive}" | shasum -a 256 -c -
mkdir -p "${source_root}/ffmpeg"
tar -xf "${ffmpeg_archive}" -C "${source_root}/ffmpeg" --strip-components=1

pushd "${source_root}/ffmpeg"
./configure \
    --prefix="${dependency_prefix}" \
    --arch=x86_64 \
    --target-os=darwin \
    --cc=clang \
    --cxx=clang++ \
    --enable-static \
    --disable-shared \
    --enable-pic \
    --enable-gpl \
    --enable-version3 \
    --enable-videotoolbox \
    --enable-audiotoolbox \
    --enable-avfoundation \
    --disable-filter=transpose_vt \
    --disable-doc \
    --disable-debug \
    --extra-cflags="${CFLAGS}" \
    --extra-cxxflags="${CXXFLAGS}" \
    --extra-ldflags="${LDFLAGS}"
make -j"$(sysctl -n hw.logicalcpu)"
make install
popd

"${dependency_prefix}/bin/ffmpeg" -version
if nm -u "${dependency_prefix}/lib/libavfilter.a" | \
        grep -q '_VTPixelRotationSessionCreate'; then
    echo "FFmpeg still imports VTPixelRotationSessionCreate" >&2
    exit 1
fi

meson setup build \
    --prefix="${work_root}/mpv-prefix" \
    --buildtype=release \
    -Dlibmpv=false \
    -Dtests=false \
    -Dcaca=disabled \
    -Dcdda=disabled \
    -Ddvda=disabled \
    -Ddvdnav=disabled \
    -Djavascript=disabled \
    -Djpeg=disabled \
    -Dlibarchive=disabled \
    -Dlibbluray=disabled \
    -Dlibcurl=disabled \
    -Dlua=luajit \
    -Dlcms2=disabled \
    -Drubberband=disabled \
    -Duchardet=disabled \
    -Dzimg=disabled \
    -Dgl=enabled \
    -Dplain-gl=enabled \
    -Dcocoa=enabled \
    -Dcoreaudio=enabled \
    -Dgl-cocoa=enabled \
    -Dvideotoolbox-gl=enabled \
    -Dvideotoolbox-pl=disabled \
    -Dvulkan=disabled \
    -Dswift-build=enabled \
    -Dmacos-cocoa-cb=enabled \
    -Dmacos-media-player=enabled \
    -Dmacos-touchbar=enabled \
    -Dobjc_args="-Wno-error=deprecated -Wno-error=deprecated-declarations" \
    -Dswift-flags="${SWIFT_FLAGS}"

meson compile -C build -j4
./build/mpv -v --no-config
