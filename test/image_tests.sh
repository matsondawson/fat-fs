#!/usr/bin/env bash
# Cross-check the driver against the standard Linux FAT tools using disk image files
# (no real drives are touched):
#   - images made by mkfs.fat are readable/writable by the driver
#   - files written by the driver are readable by mtools
#   - fsck.fat finds no problems after the driver has modified a volume
#
# usage: image_tests.sh path/to/fatimg [workdir]
set -u

FATIMG=${1:?usage: image_tests.sh path/to/fatimg [workdir]}
WORK=${2:-$(mktemp -d)}
mkdir -p "$WORK"
export MTOOLS_SKIP_CHECK=1
export MTOOLS_NO_VFAT=1

failures=0
pass() { echo "[  OK  ] $1"; }
fail() { echo "[ FAIL ] $1: $2"; failures=$((failures + 1)); }

fsck_clean() { # image
	local out
	if ! out=$(fsck.fat -n "$1" 2>&1); then
		echo "$out"
		return 1
	fi
	return 0
}

make_data() { # file bytes seed
	head -c "$2" /dev/urandom > "$1"
}

# 1. Driver-formatted image passes fsck and is readable by mtools
img="$WORK/ours.img"
rm -f "$img"
if "$FATIMG" "$img" create 40 && fsck_clean "$img" && mdir -i "$img" :: >/dev/null; then
	pass "driver format accepted by fsck.fat and mtools"
else
	fail "driver format accepted by fsck.fat and mtools" "see output above"
fi

# 2. Write a file with the driver into an mkfs.fat image, read it with mtools
img="$WORK/mkfs.img"
rm -f "$img"
mkfs.fat -F 32 -S 512 -s 1 -n TESTVOL -C "$img" 40960 >/dev/null
make_data "$WORK/small.bin" 1234
if "$FATIMG" "$img" put "$WORK/small.bin" /SMALL.BIN &&
	mtype -i "$img" ::SMALL.BIN > "$WORK/small.out" &&
	cmp -s "$WORK/small.bin" "$WORK/small.out" && fsck_clean "$img"; then
	pass "driver write -> mtools read"
else
	fail "driver write -> mtools read" "content mismatch or fsck error"
fi

# 3. mtools writes into a subdirectory, the driver reads it
make_data "$WORK/sub.bin" 70000
if mmd -i "$img" ::SUB && mcopy -i "$img" "$WORK/sub.bin" ::SUB/DATA.BIN &&
	"$FATIMG" "$img" get /SUB/DATA.BIN "$WORK/sub.out" &&
	cmp -s "$WORK/sub.bin" "$WORK/sub.out"; then
	pass "mtools write -> driver read (subdirectory)"
else
	fail "mtools write -> driver read (subdirectory)" "content mismatch"
fi

# 4. Driver mkdir + multi cluster file, verified by mtools and fsck
make_data "$WORK/big.bin" 300000
if "$FATIMG" "$img" mkdir /NEW && "$FATIMG" "$img" mkdir /NEW/DEEPER &&
	"$FATIMG" "$img" put "$WORK/big.bin" /NEW/DEEPER/BIG.BIN &&
	mcopy -n -i "$img" ::NEW/DEEPER/BIG.BIN "$WORK/big.out" &&
	cmp -s "$WORK/big.bin" "$WORK/big.out" && fsck_clean "$img"; then
	pass "driver mkdir + large file -> mtools read, fsck clean"
else
	fail "driver mkdir + large file -> mtools read, fsck clean" "see output above"
fi

# 5. Replacing a file with a shorter one frees clusters correctly
make_data "$WORK/shorter.bin" 5000
if "$FATIMG" "$img" put "$WORK/shorter.bin" /NEW/DEEPER/BIG.BIN &&
	mtype -i "$img" ::NEW/DEEPER/BIG.BIN > "$WORK/shorter.out" &&
	cmp -s "$WORK/shorter.bin" "$WORK/shorter.out" && fsck_clean "$img"; then
	pass "driver truncate/overwrite, fsck clean"
else
	fail "driver truncate/overwrite, fsck clean" "see output above"
fi

# 6. Many files: directory grows past one cluster
ok=1
for i in $(seq -w 1 40); do
	echo "file $i" > "$WORK/f.txt"
	"$FATIMG" "$img" put "$WORK/f.txt" "/NEW/F$i.TXT" || ok=0
done
count=$(mdir -b -i "$img" ::NEW 2>/dev/null | grep -c 'F[0-9]*\.TXT')
if [ $ok = 1 ] && [ "$count" = 40 ] && [ "$(mtype -i "$img" ::NEW/F37.TXT)" = "file 37" ] && fsck_clean "$img"; then
	pass "directory growth (40 files)"
else
	fail "directory growth (40 files)" "found $count files"
fi

# 7. Delete
if "$FATIMG" "$img" rm /SMALL.BIN && ! mdir -i "$img" ::SMALL.BIN >/dev/null 2>&1 && fsck_clean "$img"; then
	pass "driver rm, fsck clean"
else
	fail "driver rm, fsck clean" "file still present or fsck error"
fi

# 8. MBR partitioned image (data starts 1MiB in)
if command -v sfdisk >/dev/null; then
	img="$WORK/part.img"
	rm -f "$img"
	truncate -s 41M "$img"
	echo 'start=2048, type=c' | sfdisk -q "$img" >/dev/null 2>&1
	make_data "$WORK/part.bin" 4000
	if "$FATIMG" -p 0 "$img" format && "$FATIMG" -p 0 "$img" put "$WORK/part.bin" /PART.BIN &&
		mtype -i "$img@@1M" ::PART.BIN > "$WORK/part.out" && cmp -s "$WORK/part.bin" "$WORK/part.out" &&
		dd if="$img" of="$WORK/part-only.img" bs=1M skip=1 status=none && fsck_clean "$WORK/part-only.img"; then
		pass "MBR partition"
	else
		fail "MBR partition" "see output above"
	fi
else
	echo "[ SKIP ] MBR partition (sfdisk not found)"
fi

# 9. Deleting a file mtools gave a long name (by its short alias) must also delete
#    the long name entries; fsck reports orphans otherwise. Applies to every build.
img="$WORK/lfn.img"
rm -f "$img"
mkfs.fat -F 32 -S 512 -s 1 -C "$img" 40960 >/dev/null
echo data > "$WORK/d.txt"
lfn_mcopy() { env -u MTOOLS_NO_VFAT mcopy "$@"; }
if lfn_mcopy -i "$img" "$WORK/d.txt" "::Another Long Name.dat" &&
	"$FATIMG" "$img" rm /ANOTHE~1.DAT && fsck_clean "$img" &&
	! mdir -i "$img" "::Another Long Name.dat" >/dev/null 2>&1; then
	pass "delete mtools long-named file by alias, no orphans"
else
	fail "delete mtools long-named file by alias, no orphans" "see output above"
fi

if "$FATIMG" --features | grep -qx lfn; then
	# 10. Driver creates long names; mtools reads them.
	make_data "$WORK/report.bin" 20000
	if "$FATIMG" "$img" mkdir "/My Documents" &&
		"$FATIMG" "$img" put "$WORK/report.bin" "/My Documents/Quarterly Report 2026.txt" &&
		env -u MTOOLS_NO_VFAT mtype -i "$img" "::My Documents/Quarterly Report 2026.txt" > "$WORK/report.out" &&
		cmp -s "$WORK/report.bin" "$WORK/report.out" &&
		env -u MTOOLS_NO_VFAT mdir -i "$img" "::My Documents" | grep -q "Quarterly Report 2026.txt" &&
		fsck_clean "$img"; then
		pass "driver long names -> mtools"
	else
		fail "driver long names -> mtools" "see output above"
	fi

	# 11. mtools creates long names; the driver lists and reads them (any case).
	make_data "$WORK/mixed.bin" 3000
	if lfn_mcopy -i "$img" "$WORK/mixed.bin" "::Mixed Case Name.TXT" &&
		"$FATIMG" "$img" ls / | grep -q "^Mixed Case Name.TXT" &&
		"$FATIMG" "$img" get "/mixed case name.txt" "$WORK/mixed.out" &&
		cmp -s "$WORK/mixed.bin" "$WORK/mixed.out"; then
		pass "mtools long names -> driver"
	else
		fail "mtools long names -> driver" "listing or content mismatch"
	fi

	# 12. Many long names (runs straddle clusters), then delete some, fsck clean.
	ok=1
	for i in $(seq -w 1 30); do
		echo "entry $i" > "$WORK/f.txt"
		"$FATIMG" "$img" put "$WORK/f.txt" "/My Documents/A fairly long file name, number $i.txt" || ok=0
	done
	for i in $(seq -w 1 3 30); do
		"$FATIMG" "$img" rm "/My Documents/A fairly long file name, number $i.txt" || ok=0
	done
	count=$(env -u MTOOLS_NO_VFAT mdir -i "$img" "::My Documents" 2>/dev/null | grep -c 'A fairly long file name')
	if [ $ok = 1 ] && [ "$count" = 20 ] &&
		[ "$(env -u MTOOLS_NO_VFAT mtype -i "$img" "::My Documents/A fairly long file name, number 29.txt")" = "entry 29" ] &&
		fsck_clean "$img"; then
		pass "many long names + deletes, fsck clean"
	else
		fail "many long names + deletes, fsck clean" "found $count files"
	fi
else
	echo "[ SKIP ] long name cross-checks (fatimg built without long file names)"
fi

if "$FATIMG" --features | grep -qx fat12_16; then
	# 13. FAT16 and FAT12: images from mkfs.fat and from the driver, written by the
	#     driver (fixed root directory, subdirectories, multi cluster files) and
	#     checked by mtools and fsck.fat.
	for bits in 16 12; do
		if [ $bits = 16 ]; then size_kb=32768; else size_kb=4096; fi
		for maker in mkfs driver; do
			img="$WORK/fat$bits-$maker.img"
			rm -f "$img"
			if [ $maker = mkfs ]; then
				mkfs.fat -F $bits -S 512 -s 2 -n TESTVOL -C "$img" $size_kb >/dev/null
			else
				"$FATIMG" -t $bits "$img" create $((size_kb / 1024)) || fail "FAT$bits format" "fatimg create"
			fi
			name="FAT$bits ($maker image)"
			if ! "$FATIMG" "$img" info | grep -q "^FAT$bits,"; then
				fail "$name" "not mounted as FAT$bits"
				continue
			fi
			# 900 KB: on FAT12 with 1 KB clusters its chain crosses the entries split
			# across FAT sectors (clusters 341 and 682)
			make_data "$WORK/a.bin" 900000
			make_data "$WORK/b.bin" 3000
			ok=1
			"$FATIMG" "$img" put "$WORK/a.bin" /A.BIN || ok=0
			"$FATIMG" "$img" mkdir /DIR || ok=0
			"$FATIMG" "$img" mkdir /DIR/SUB || ok=0
			"$FATIMG" "$img" put "$WORK/b.bin" /DIR/SUB/B.BIN || ok=0
			for i in $(seq -w 1 60); do
				echo "root $i" > "$WORK/f.txt"
				"$FATIMG" "$img" put "$WORK/f.txt" "/R$i.TXT" || ok=0
			done
			"$FATIMG" "$img" rm /R30.TXT || ok=0
			# mtools writes, the driver reads
			mcopy -i "$img" "$WORK/b.bin" ::DIR/M.BIN || ok=0
			"$FATIMG" "$img" get /DIR/M.BIN "$WORK/m.out" && cmp -s "$WORK/b.bin" "$WORK/m.out" || ok=0
			count=$(mdir -b -i "$img" :: 2>/dev/null | grep -c 'R[0-9]*\.TXT')
			if [ $ok = 1 ] && [ "$count" = 59 ] &&
				mtype -i "$img" ::A.BIN > "$WORK/a.out" && cmp -s "$WORK/a.bin" "$WORK/a.out" &&
				mtype -i "$img" ::DIR/SUB/B.BIN > "$WORK/b.out" && cmp -s "$WORK/b.bin" "$WORK/b.out" &&
				[ "$(mtype -i "$img" ::R45.TXT)" = "root 45" ] && fsck_clean "$img"; then
				pass "$name: driver writes, mtools reads, fsck clean"
			else
				fail "$name: driver writes, mtools reads, fsck clean" "found $count root files"
			fi
		done
	done
else
	# 13. Without FAT12/16 support, such volumes are refused rather than misread
	img="$WORK/fat16-refused.img"
	rm -f "$img"
	mkfs.fat -F 16 -S 512 -s 2 -C "$img" 32768 >/dev/null
	if ! "$FATIMG" "$img" info >/dev/null 2>&1; then
		pass "FAT16 volume refused without FAT12/16 support"
	else
		fail "FAT16 volume refused without FAT12/16 support" "it mounted"
	fi
fi

echo
if [ $failures -ne 0 ]; then
	echo "$failures image test(s) failed (images kept in $WORK)"
	exit 1
fi
echo "all image tests passed"
