#!/usr/bin/env bash
# Structural validation for a HiByOS/TempoTec UPT. This cannot prove hardware
# compatibility, but it catches corrupt ISO output, broken chunk/hash chains,
# changed kernels, and a missing Sonix install before an artifact is published.
#
# --stock STOCK.upt compares the image with the stock update it was made from:
# every file of the package except the rootfs payload -- the OTA config, the
# compatibility marker newer stock packages carry, the kernel chunks and their
# chain, the done-marker -- must be byte-for-byte the stock file, and
# ota_update.in may differ only in the rootfs size and MD5.
set -euo pipefail

usage() {
	echo "Usage: verify_upt.sh IMAGE.upt [--sonix-v1] [--kernel-sha256 HASH] [--stock STOCK.upt]" >&2
	exit 2
}

[ "$#" -ge 1 ] || usage
UPT="$1"
shift
EXPECT_SONIX=false
EXPECTED_KERNEL=""
STOCK_UPT=""
while [ "$#" -gt 0 ]; do
	case "$1" in
		--sonix-v1) EXPECT_SONIX=true ;;
		--kernel-sha256)
			shift
			[ "$#" -gt 0 ] || usage
			EXPECTED_KERNEL="$1"
			;;
		--stock)
			shift
			[ "$#" -gt 0 ] || usage
			STOCK_UPT="$1"
			;;
		*) usage ;;
	esac
	shift
done

for tool in 7z md5sum sha256sum awk sed grep find sort cmp unsquashfs; do
	command -v "$tool" >/dev/null 2>&1 || { echo "Error: $tool is required." >&2; exit 1; }
done
[ -f "$UPT" ] || { echo "Error: $UPT does not exist." >&2; exit 1; }
UPT="$(cd "$(dirname "$UPT")" && pwd)/$(basename "$UPT")"
if [ -n "$STOCK_UPT" ]; then
	[ -f "$STOCK_UPT" ] || { echo "Error: $STOCK_UPT does not exist." >&2; exit 1; }
	STOCK_UPT="$(cd "$(dirname "$STOCK_UPT")" && pwd)/$(basename "$STOCK_UPT")"
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sonix-verify-upt.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

7z t "$UPT" >/dev/null
7z x "$UPT" -o"$WORK/iso" -y >/dev/null
OTA="$WORK/iso/ota_v0"
[ -f "$OTA/ota_update.in" ] || { echo "Error: image has no ota_v0/ota_update.in." >&2; exit 1; }

metadata_for() {
	local wanted="$1" key="$2"
	awk -F= -v wanted="$wanted" -v key="$key" '
		{sub(/\r$/, "", $0)}
		$1 == "img_name" { active = ($2 == wanted) }
		active && $1 == key { print $2; exit }
	' "$OTA/ota_update.in"
}

verify_payload() {
	local stem="$1" label="$2"
	local declared_size declared_md5 full actual_size actual_md5 manifest
	declared_size="$(metadata_for "$stem" img_size)"
	declared_md5="$(metadata_for "$stem" img_md5 | tr '[:upper:]' '[:lower:]')"
	[ -n "$declared_size" ] && [[ "$declared_size" =~ ^[0-9]+$ ]] || {
		echo "Error: invalid $label size in ota_update.in." >&2; exit 1;
	}
	[[ "$declared_md5" =~ ^[0-9a-f]{32}$ ]] || {
		echo "Error: invalid $label MD5 in ota_update.in." >&2; exit 1;
	}

	mapfile -t chunks < <(find "$OTA" -maxdepth 1 -type f -regextype posix-extended \
		-regex ".*/${stem}\.[0-9]{4}\.[0-9a-fA-F]{32}" -print | sort)
	[ "${#chunks[@]}" -gt 0 ] || { echo "Error: no $label chunks found." >&2; exit 1; }

	full="$WORK/$stem"
	: > "$full"
	for chunk in "${chunks[@]}"; do cat "$chunk" >> "$full"; done
	actual_size="$(wc -c < "$full" | tr -d ' ')"
	actual_md5="$(md5sum "$full" | awk '{print $1}')"
	[ "$actual_size" = "$declared_size" ] || {
		echo "Error: $label is $actual_size bytes; metadata says $declared_size." >&2; exit 1;
	}
	[ "$actual_md5" = "$declared_md5" ] || {
		echo "Error: $label MD5 is $actual_md5; metadata says $declared_md5." >&2; exit 1;
	}

	manifest="$OTA/ota_md5_${stem}.${declared_md5}"
	[ -f "$manifest" ] || { echo "Error: $label MD5-chain file is missing." >&2; exit 1; }
	mapfile -t chain < <(tr -d '\r ' < "$manifest" | sed '/^$/d' | tr '[:upper:]' '[:lower:]')
	[ "${#chain[@]}" = "${#chunks[@]}" ] || {
		echo "Error: $label has ${#chunks[@]} chunks but ${#chain[@]} chain entries." >&2; exit 1;
	}

	local previous="$declared_md5" index=0 basename suffix chunk_md5
	for chunk in "${chunks[@]}"; do
		basename="$(basename "$chunk")"
		suffix="${basename##*.}"
		suffix="$(printf '%s' "$suffix" | tr '[:upper:]' '[:lower:]')"
		[ "$suffix" = "$previous" ] || {
			echo "Error: $basename names previous MD5 $suffix; expected $previous." >&2; exit 1;
		}
		chunk_md5="$(md5sum "$chunk" | awk '{print $1}')"
		[ "${chain[$index]}" = "$chunk_md5" ] || {
			echo "Error: chain entry $index for $label is ${chain[$index]}; chunk is $chunk_md5." >&2; exit 1;
		}
		previous="$chunk_md5"
		index=$((index + 1))
	done

	printf 'Verified %-7s %10s bytes  md5 %s  (%s chunks)\n' "$label" "$actual_size" "$actual_md5" "${#chunks[@]}"
}

verify_payload xImage kernel
verify_payload rootfs.squashfs rootfs

KERNEL_SHA256="$(sha256sum "$WORK/xImage" | awk '{print $1}')"
if [ -n "$EXPECTED_KERNEL" ] && [ "$KERNEL_SHA256" != "$EXPECTED_KERNEL" ]; then
	echo "Error: output kernel SHA-256 $KERNEL_SHA256 differs from stock $EXPECTED_KERNEL." >&2
	exit 1
fi
printf 'Kernel SHA-256: %s\n' "$KERNEL_SHA256"

# The OTA config at the root and, from the V1 1.3 stock package on, a
# compatibility marker beside ota_update.in (compatibility=<tag>). Nothing in
# the rootfs or the kernel reads the marker; it is there for whatever accepts
# the update, so it is reported, checked for shape, and -- with --stock --
# required to be the stock package's own file.
if [ -f "$WORK/iso/ota_config.in" ]; then
	printf 'OTA config: %s\n' "$(tr -d '\r' < "$WORK/iso/ota_config.in" | sed '/^$/d' | paste -sd ' ' -)"
fi
COMPAT_FILE="$OTA/ota_compatibility.in"
if [ -f "$COMPAT_FILE" ]; then
	COMPATIBILITY="$(tr -d '\r' < "$COMPAT_FILE" | sed -n 's/^compatibility=//p' | head -n 1)"
	[[ "$COMPATIBILITY" =~ ^[A-Za-z0-9._-]+$ ]] || {
		echo "Error: ota_v0/ota_compatibility.in carries no usable compatibility= line." >&2; exit 1;
	}
	printf 'Compatibility: %s\n' "$COMPATIBILITY"
else
	printf 'Compatibility: none (no ota_v0/ota_compatibility.in; packages before V1 1.3 have none)\n'
fi

# The firmware's own idea of its version, from the product entry of the stock
# usr/resource/config.json, which the packer leaves in place.
if command -v python3 >/dev/null 2>&1 && [ -f "$SCRIPT_DIR/extract_base_build.py" ]; then
	if unsquashfs -cat "$WORK/rootfs.squashfs" usr/resource/config.json > "$WORK/config.json" 2>/dev/null; then
		PRODUCT_VERSION="$(python3 "$SCRIPT_DIR/extract_base_build.py" "$WORK/config.json")"
		printf 'TempoTec product version: %s\n' "${PRODUCT_VERSION:-unknown}"
	else
		printf 'TempoTec product version: unknown (no usr/resource/config.json)\n'
	fi
fi

if [ -n "$STOCK_UPT" ]; then
	7z x "$STOCK_UPT" -o"$WORK/stock" -y >/dev/null
	[ -f "$WORK/stock/ota_v0/ota_update.in" ] || {
		echo "Error: $STOCK_UPT has no ota_v0/ota_update.in." >&2; exit 1;
	}
	# Every file but the rootfs payload, which is the one part allowed to change.
	package_files() {
		(cd "$1" && find . -type f -print | sed 's|^\./||' | LC_ALL=C sort) |
			grep -vE '^ota_v0/(rootfs\.squashfs\.[0-9]{4}\.[0-9a-fA-F]{32}|ota_md5_rootfs\.squashfs\.[0-9a-fA-F]{32}|ota_update\.in)$' || true
	}
	package_files "$WORK/stock" > "$WORK/stock.files"
	package_files "$WORK/iso" > "$WORK/image.files"
	if ! cmp -s "$WORK/stock.files" "$WORK/image.files"; then
		echo "Error: the package files outside the rootfs differ from the stock update:" >&2
		diff "$WORK/stock.files" "$WORK/image.files" | sed -n 's/^</  only in stock:/p; s/^>/  only in image:/p' >&2
		exit 1
	fi
	while IFS= read -r file; do
		cmp -s "$WORK/stock/$file" "$WORK/iso/$file" || {
			echo "Error: $file differs from the stock update's." >&2; exit 1;
		}
	done < "$WORK/stock.files"

	# The rootfs block's size and MD5 are the only values the packer writes;
	# the version line, the kernel block, the order and the blank lines are the
	# stock file's.
	normalize_update() {
		tr -d '\r' < "$1" | awk -F= '
			$1 == "img_name" { rootfs = ($2 == "rootfs.squashfs") }
			rootfs && ($1 == "img_size" || $1 == "img_md5") { print $1 "=<rootfs>"; next }
			{ print }
		'
	}
	normalize_update "$WORK/stock/ota_v0/ota_update.in" > "$WORK/stock.update"
	normalize_update "$OTA/ota_update.in" > "$WORK/image.update"
	if ! cmp -s "$WORK/stock.update" "$WORK/image.update"; then
		echo "Error: ota_update.in differs from the stock layout beyond the rootfs size and MD5:" >&2
		diff "$WORK/stock.update" "$WORK/image.update" >&2 || true
		exit 1
	fi
	printf 'Verified against stock %s: %s package files byte-identical, ota_update.in differs only in the rootfs size and MD5.\n' \
		"$(basename "$STOCK_UPT")" "$(wc -l < "$WORK/stock.files" | tr -d ' ')"
fi

if $EXPECT_SONIX; then
	LISTING="$WORK/rootfs.list"
	unsquashfs -ll "$WORK/rootfs.squashfs" > "$LISTING"
	grep -qE 'squashfs-root/usr/bin/sonix_player$' "$LISTING" || {
		echo "Error: rootfs has no usr/bin/sonix_player." >&2; exit 1;
	}
	if grep -qE 'squashfs-root/usr/bin/hiby_player$' "$LISTING"; then
		echo "Error: stock usr/bin/hiby_player remains in the rootfs." >&2
		exit 1
	fi
	INFO="$WORK/system-info.json"
	unsquashfs -cat "$WORK/rootfs.squashfs" usr/resource/sonix/components/system-info.json > "$INFO"
	grep -qE '"device-name"[[:space:]]*:[[:space:]]*"TempoTec V1"' "$INFO" || {
		echo "Error: system-info.json does not identify TempoTec V1." >&2; exit 1;
	}

	# Every https:// connection the player opens -- Tidal, Qobuz, podcasts,
	# radio, Last.fm, Subsonic -- is verified against this bundle, and strict
	# mode refuses to connect without one. The failure it causes on the device
	# ("no reply", or nothing at all) does not say a file is missing, so it is
	# checked here instead of being found in the field.
	CA_BUNDLE="$WORK/cert.pem"
	if ! unsquashfs -cat "$WORK/rootfs.squashfs" etc/ssl/cert.pem > "$CA_BUNDLE" 2>/dev/null; then
		: > "$CA_BUNDLE"
	fi
	grep -q '^-----BEGIN CERTIFICATE-----' "$CA_BUNDLE" || {
		echo "Error: rootfs has no usable etc/ssl/cert.pem; https streaming would fail." >&2
		exit 1
	}
	printf 'Verified Sonix player and TempoTec V1 identity in rootfs.\n'
	printf 'Verified CA bundle: %s certificates, %s bytes.\n' \
		"$(grep -c '^-----BEGIN CERTIFICATE-----' "$CA_BUNDLE")" \
		"$(wc -c < "$CA_BUNDLE" | tr -d ' ')"
fi

printf 'UPT validation passed: %s\n' "$UPT"
