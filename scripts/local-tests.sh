#!/usr/bin/env bash
# Runs every test and build on the committed HEAD, then reports the result to
# GitHub as the "local-tests" commit status. Branch protection on main requires
# that status, so a pull request can only be merged once its latest commit has
# passed here. Nothing runs on GitHub.
#
#   scripts/local-tests.sh               test HEAD and report the result
#   scripts/local-tests.sh --no-report   test only
#
# HEAD must be pushed (the status is attached to the commit on GitHub) and the
# working tree clean (so what was tested is exactly the commit).
#
# Runs: host tests with gcc and clang (warnings as errors, ctest including the
# mkfs.fat/mtools/fsck.fat image checks), the Cortex-M0+ and M4 library in
# every option combination, and the Pico and Pico 2 firmware (pico/pico.sh).
# Needs cmake, ninja, gcc, clang, dosfstools, mtools, arm-none-eabi-gcc, docker
# and gh.
set -uo pipefail

ROOT=$(git rev-parse --show-toplevel) || exit 2
cd "$ROOT" || exit 2

report=1
case "${1:-}" in
	--no-report) report=0 ;;
	"") ;;
	*)
		sed -n '2,12p' "$0"
		exit 2
		;;
esac

if [ -n "$(git status --porcelain)" ]; then
	echo "Commit or stash your changes first: the result is reported for the commit." >&2
	exit 2
fi
for tool in cmake ninja gcc clang++ mkfs.fat fsck.fat mcopy arm-none-eabi-gcc docker gh; do
	if ! command -v "$tool" > /dev/null; then
		echo "Missing $tool" >&2
		exit 2
	fi
done

sha=$(git rev-parse HEAD)
if [ $report = 1 ] && ! gh api "repos/{owner}/{repo}/commits/$sha" --silent 2> /dev/null; then
	echo "Push $(git rev-parse --short HEAD) first: GitHub can only take a status for a commit it has." >&2
	exit 2
fi

out=build-local-tests
rm -rf "$out"
mkdir -p "$out"
failed=()

# step NAME COMMAND...: runs the command with its output in $out/NAME.log
step() {
	local name=$1
	shift
	printf '%-58s' "$name"
	if "$@" > "$out/$name.log" 2>&1; then
		echo ok
	else
		echo "FAILED (see $out/$name.log)"
		failed+=("$name")
	fi
}

host() ( # compiler; a subshell, so CC and CXX don't leak into the other steps
	dir=$out/host-$1
	if [ "$1" = clang ]; then
		export CC=clang CXX=clang++
	else
		export CC=gcc CXX=g++
	fi
	cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_COMPILE_WARNING_AS_ERROR=ON &&
		cmake --build "$dir" -j &&
		ctest --test-dir "$dir" --output-on-failure -j "$(nproc)"
)

arm() { # cpu lfn cache fat12_16
	local dir=$out/arm-$1-lfn$2-cache$3-fat1216$4
	cmake -S . -B "$dir" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DFAT_ARM_CPU="$1" \
		-DFAT_LONG_FILE_NAMES="$2" -DFAT_BLOCK_CACHE="$3" -DFAT_FAT12_16="$4" \
		-DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_COMPILE_WARNING_AS_ERROR=ON &&
		cmake --build "$dir" &&
		arm-none-eabi-size -t "$dir/libfat.a" | tail -1
}

firmware() { # board
	pico/pico.sh --board "$1" build && ! grep -q 'warning:' "$out/firmware-$1.log"
}

step host-gcc host gcc
step host-clang host clang
for cpu in cortex-m0plus cortex-m4; do
	for lfn in OFF ON; do
		for cache in OFF ON; do
			for fat1216 in OFF ON; do
				step "arm-$cpu-lfn$lfn-cache$cache-fat1216$fat1216" arm $cpu $lfn $cache $fat1216
			done
		done
	done
done
step firmware-pico firmware pico
step firmware-pico2 firmware pico2

# The tests must have left the tree as it was
if [ -n "$(git status --porcelain)" ]; then
	echo "The working tree changed during the run:" >&2
	git status --short >&2
	failed+=("clean-tree")
fi

if [ ${#failed[@]} = 0 ]; then
	state=success
	description="All passed"
	echo "All passed"
else
	state=failure
	description="Failed: ${failed[*]}"
	description=${description:0:140}
	echo "${#failed[@]} failed: ${failed[*]}"
fi

if [ $report = 1 ]; then
	if gh api "repos/{owner}/{repo}/statuses/$sha" -f state="$state" -f context=local-tests \
		-f description="$description" --silent; then
		echo "Reported local-tests: $state for $(git rev-parse --short HEAD)"
	else
		echo "Couldn't report the result to GitHub" >&2
		exit 1
	fi
fi

[ $state = success ]
