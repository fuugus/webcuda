#!/usr/bin/env bash
# webcuda build script.
#
#   ./build.sh                 build every platform buildable on this host
#   ./build.sh linux           build the Linux binary (Linux host)
#   ./build.sh windows         build the Windows binary (Windows host, Git Bash)
#   ./build.sh linux --test    additionally run the self-test (needs display+GPU)
#
# Native toolchains only: nvcc cannot cross-compile (Windows CUDA requires
# MSVC), so non-host platforms are built by CI (.github/workflows/build.yml)
# or by running this script on that platform. macOS is unsupported: CUDA does
# not exist there (see README).
set -euo pipefail
cd "$(dirname "$0")"

CEF_VERSION="148.0.10+g7ee53f5+chromium-148.0.7778.218"
CEF_BASE="https://cef-builds.spotifycdn.com"

# ---------------------------------------------------------------------------
host_platform() {
  case "$(uname -s)" in
    Linux*)                     echo linux ;;
    MINGW*|MSYS*|CYGWIN*)       echo windows ;;
    Darwin*)                    echo macos ;;
    *)                          echo unknown ;;
  esac
}

fetch_cef() {  # $1 = linux|windows
  local dist
  case "$1" in
    linux)   dist="cef_binary_${CEF_VERSION}_linux64_minimal" ;;
    windows) dist="cef_binary_${CEF_VERSION}_windows64_minimal" ;;
  esac
  local dir="third_party/cef-$1"
  # adopt a legacy third_party/cef checkout as the host dist
  if [ "$1" = "$(host_platform)" ] && [ -d third_party/cef/cmake ] && \
     [ ! -L third_party/cef ] && [ ! -d "$dir" ]; then
    mv third_party/cef "$dir"
    ln -s "cef-$1" third_party/cef
  fi
  if [ -d "$dir/cmake" ]; then
    echo "[build] CEF for $1 already present ($dir)"
    return
  fi
  mkdir -p third_party
  local url="${CEF_BASE}/$(printf '%s' "$dist" | sed 's/+/%2B/g').tar.bz2"
  echo "[build] downloading CEF for $1 ..."
  curl -fL --retry 3 -o "third_party/cef-$1.tar.bz2" "$url"
  tar xjf "third_party/cef-$1.tar.bz2" -C third_party
  mv "third_party/$dist" "$dir"
  rm "third_party/cef-$1.tar.bz2"
  # legacy path used by older docs: keep third_party/cef pointing at the host dist
  if [ "$1" = "$(host_platform)" ] && [ ! -e third_party/cef ]; then
    ln -s "cef-$1" third_party/cef
  fi
}

cuda_archs() {
  # native needs a visible GPU (fails on CI); fall back to a broad set
  if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then
    echo native
  else
    echo "75;86;89;120"
  fi
}

find_nvcc() {
  # prefer the newest toolkit over a possibly stale distro /usr/bin/nvcc
  local best=""
  for c in /usr/local/cuda*/bin/nvcc; do [ -x "$c" ] && best="$c"; done
  if [ -z "$best" ] && command -v nvcc >/dev/null 2>&1; then best="$(command -v nvcc)"; fi
  echo "$best"
}

build_linux() {
  fetch_cef linux
  local nvcc; nvcc="$(find_nvcc)"
  [ -n "$nvcc" ] || { echo "[build] ERROR: nvcc not found — install the CUDA toolkit"; exit 1; }
  echo "[build] linux: nvcc=$nvcc archs=$(cuda_archs)"
  cmake -B build-linux -DCMAKE_BUILD_TYPE=Release \
        -DCEF_ROOT="$PWD/third_party/cef-linux" \
        -DCMAKE_CUDA_COMPILER="$nvcc" \
        -DCMAKE_CUDA_ARCHITECTURES="$(cuda_archs)"
  cmake --build build-linux -j "$(nproc)"
  echo "[build] linux binary: build-linux/webcuda"
}

build_windows() {
  fetch_cef windows
  echo "[build] windows: letting CMake pick the Visual Studio toolchain"
  cmake -B build-windows \
        -DCEF_ROOT="$PWD/third_party/cef-windows" \
        -DCMAKE_CUDA_ARCHITECTURES="$(cuda_archs)"
  cmake --build build-windows --config Release -j
  echo "[build] windows binary: build-windows/Release/webcuda.exe"
}

run_test() {
  case "$1" in
    linux)
      echo "[build] running self-test ..."
      (cd build-linux && ./webcuda --selftest --shot selftest.ppm)
      ;;
    windows)
      echo "[build] running self-test ..."
      (cd build-windows/Release && ./webcuda.exe --selftest --shot selftest.ppm)
      ;;
  esac
}

# ---------------------------------------------------------------------------
HOST="$(host_platform)"
TARGETS=()
RUN_TEST=0
for arg in "$@"; do
  case "$arg" in
    linux|windows) TARGETS+=("$arg") ;;
    macos) echo "[build] macOS is unsupported: no CUDA on Apple platforms"; exit 1 ;;
    --test) RUN_TEST=1 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "[build] unknown argument: $arg (try --help)"; exit 1 ;;
  esac
done

# default: everything buildable on this host (native toolchains only)
if [ ${#TARGETS[@]} -eq 0 ]; then
  TARGETS=("$HOST")
  echo "[build] no platform given — building host platform: $HOST"
  echo "[build] (other platforms build via CI or by running this script there)"
fi

for t in "${TARGETS[@]}"; do
  if [ "$t" != "$HOST" ]; then
    echo "[build] ERROR: cannot build '$t' on a $HOST host — nvcc does not"
    echo "        cross-compile. Run this script on $t or rely on the CI matrix."
    exit 1
  fi
  "build_$t"
  [ "$RUN_TEST" = 1 ] && run_test "$t"
done
echo "[build] done."
