#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 <ffmpeg-source> <build-directory> [threadless|pthread]" >&2
    exit 2
fi

source_dir=$(cd "$1" && pwd)
build_dir=$2
thread_mode=${3:-pthread}
port_dir=$(cd "$(dirname "$0")" && pwd)
cc="$port_dir/neutrino-cc"
bearssl_root=$(cd "$port_dir/../../../neutrino-packages/bearssl" && pwd)

if [[ ! -x "$source_dir/configure" ]]; then
    echo "FFmpeg configure script not found under $source_dir" >&2
    exit 1
fi
if [[ "$thread_mode" != threadless && "$thread_mode" != pthread ]]; then
    echo "thread mode must be 'threadless' or 'pthread'" >&2
    exit 2
fi

mkdir -p "$build_dir"
cd "$build_dir"

thread_flags=(--disable-pthreads)
if [[ "$thread_mode" == pthread ]]; then
    thread_flags=(--enable-pthreads)
fi

"$source_dir/configure" \
    --prefix=/ \
    --libdir=/library \
    --shlibdir=/library \
    --incdir=/include \
    --target-os=none \
    --arch=x86_64 \
    --enable-cross-compile \
    --cc="$cc" \
    --ar=x86_64-elf-ar \
    --ranlib=x86_64-elf-ranlib \
    --strip=x86_64-elf-strip \
    --enable-shared \
    --enable-static \
    --disable-autodetect \
    --disable-doc \
    --disable-debug \
    --enable-network \
    --disable-devices \
    --disable-hwaccels \
    --x86asmexe=nasm \
    --disable-everything \
    --enable-ffmpeg \
    --enable-ffplay \
    --enable-ffprobe \
    --enable-sdl2 \
    --enable-avcodec \
    --enable-avfilter \
    --enable-avformat \
    --enable-avutil \
    --enable-swresample \
    --enable-swscale \
    --enable-bearssl \
    --enable-protocol=file,http,https,tcp,tls \
    --enable-demuxer=aac,matroska,mov,mp3 \
    --enable-decoder=aac \
    --enable-decoder=h264 \
    --enable-decoder=mp3 \
    --enable-decoder=vp8 \
    --enable-decoder=vp9 \
    --enable-parser=aac \
    --enable-parser=h264 \
    --enable-parser=mpegaudio \
    --enable-parser=vp8 \
    --enable-parser=vp9 \
    --enable-muxer=adts \
    --enable-muxer=mov \
    --enable-muxer=mp3 \
    --enable-muxer=mp4 \
    --enable-muxer=rawvideo \
    --enable-muxer=null \
    --enable-bsf=aac_adtstoasc \
    --enable-bsf=h264_mp4toannexb \
    --enable-filter=aresample \
    --enable-filter=format \
    --enable-filter=null \
    --enable-filter=scale \
    "${thread_flags[@]}" \
    --extra-cflags="-D_POSIX_THREADS=200809L -D_WANT_IO_C99_FORMATS -DFFPLAY_NEUTRINO_SOFTWARE_RENDERER -I$bearssl_root/include/bearssl" \
    --extra-ldflags="-L$bearssl_root/library"
