#!/usr/bin/env bash
# Raspberry Pi Pico (RP2040) and Pico 2 (RP2350) test firmware; everything runs in Docker.
#
#   pico/pico.sh [options] build            Build fat_pico_tests
#   pico/pico.sh [options] flash            Flash it. The board must be connected.
#   pico/pico.sh monitor [seconds]          Print the board's USB serial output, default 60 s
#
# Options:
#   --board pico|pico2       Defaults to pico, which also runs on a Pico W
#   -D<VAR>=<value>          Passed to CMake, e.g. -DFAT_PICO_SD_CS=9 or -DFAT_PICO_SD_WRITE_TEST=ON;
#                            pico/CMakeLists.txt lists the SD card's settings
#
# Builds go into pico/build/<board>. CMake keeps -D settings in its cache there until changed.
set -euo pipefail

IMAGE=fat_pico
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BOARD=pico
CMAKE_ARGS=()
while true; do
	case "${1:-}" in
		--board)
			BOARD=${2:?--board needs pico or pico2}
			shift 2
			;;
		-D*)
			CMAKE_ARGS+=("$1")
			shift
			;;
		*) break ;;
	esac
done
case "$BOARD" in
	pico | pico2) ;;
	*)
		echo "Unknown board $BOARD, expected pico or pico2" >&2
		exit 1
		;;
esac
BUILD=pico/build/$BOARD

in_docker() {
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT:/src" -w /src "$IMAGE" "$@"
}

case "${1:-}" in
	build)
		docker build -t "$IMAGE" "$ROOT/pico"
		in_docker cmake -S pico -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD="$BOARD" \
			${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}
		in_docker cmake --build "$BUILD"
		in_docker arm-none-eabi-size "$BUILD/fat_pico_tests.elf"
		;;
	flash)
		load() {
			docker run --rm --privileged -v /dev/bus/usb:/dev/bus/usb -v "$ROOT:/src" -w /src "$IMAGE" \
				picotool load "$@" -v -x "$BUILD/fat_pico_tests.uf2"
		}
		# -f reboots a running board into BOOTSEL, but the container often misses it coming back as the
		# bootloader. A second container finds it.
		load -f || load
		;;
	monitor)
		seconds=${2:-60}
		device=$(ls /dev/ttyACM* | head -n1)
		docker run --rm --device "$device" "$IMAGE" \
			sh -c "stty -F $device raw -echo; timeout $seconds cat $device" || true
		;;
	*)
		sed -n '2,13p' "$0"
		exit 1
		;;
esac
