#!/usr/bin/env bash
# Locate the official v1.upt in a downloaded file/folder and put the names the
# packer expects beside it. Archives from TempoTec are accepted as well as the
# Google Drive folder containing v1.upt directly, and so is an archive that was
# split into pieces to fit a GitHub upload (v1.zip.part1 + v1.zip.part2, or
# 7-Zip volumes such as v1_upload.7z.001 + v1_upload.7z.002).
set -euo pipefail

usage() {
	cat >&2 <<'EOF'
Usage: prepare_v1_stock.sh DOWNLOAD_PATH [PACKER_DIRECTORY]

DOWNLOAD_PATH may be an official v1.upt, a directory downloaded from the
TempoTec Google Drive folder, the archive distributed for a release, or the
first piece of an archive split into pieces (NAME.part1, NAME.001); the other
pieces must sit beside it.
EOF
	exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
command -v 7z >/dev/null 2>&1 || { echo "Error: 7z is required." >&2; exit 1; }

SOURCE="$1"
PACKER_DIR="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
[ -e "$SOURCE" ] || { echo "Error: $SOURCE does not exist." >&2; exit 1; }
mkdir -p "$PACKER_DIR"
PACKER_DIR="$(cd "$PACKER_DIR" && pwd)"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/sonix-v1-stock.XXXXXX")"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

is_v1_upt() {
	local file="$1"
	[ -f "$file" ] || return 1
	# The update must be an ISO understood by 7-Zip and carry the OTA metadata,
	# not merely be any file somebody happened to call v1.upt.
	7z l "$file" 2>/dev/null | tr '\\' '/' | grep -E 'ota_v0/ota_update\.in$' >/dev/null
}

find_named_upt() {
	local root="$1"
	find "$root" -type f -iname 'v1.upt' -print -quit 2>/dev/null
}

# ---------------------------------------------------------------------------
# Split archives
#
# GitHub refuses files over 25 MB from the browser, so a stock update lands in
# the repository in pieces: plain byte splits of one archive, named
# NAME.part1, NAME.part2, ... or NAME.001, NAME.002, ... Neither piece opens
# on its own -- the first holds the start of the archive, the last its central
# directory -- so the pieces are put back together before anything looks
# inside. 7-Zip would join numbered volumes by itself, but not .partN ones;
# joining both kinds here keeps the two on one path.
# ---------------------------------------------------------------------------

# Prints "STEM<TAB>NUMBER" for a file named like a piece, nothing otherwise.
piece_of() {
	local name suffix
	name="$(basename "$1")"
	case "$name" in
		*.*) ;;
		*) return 1 ;;
	esac
	suffix="${name##*.}"
	if [[ "$suffix" =~ ^[Pp][Aa][Rr][Tt]([0-9]+)$ ]]; then
		printf '%s\t%s\n' "${name%.*}" "$((10#${BASH_REMATCH[1]}))"
	elif [[ "$suffix" =~ ^[0-9]{3,}$ ]]; then
		printf '%s\t%s\n' "${name%.*}" "$((10#$suffix))"
	else
		return 1
	fi
}

# The same naming style, so NAME.part2 never joins NAME.001.
same_style() {
	local a="${1##*.}" b="${2##*.}"
	if [[ "$a" =~ ^[Pp][Aa][Rr][Tt][0-9]+$ ]]; then
		[[ "$b" =~ ^[Pp][Aa][Rr][Tt][0-9]+$ ]]
	else
		[[ "$b" =~ ^[0-9]{3,}$ ]]
	fi
}

# Joins the pieces PIECE belongs to into OUT_DIR/STEM and prints that path.
#
# With ONLY_FIRST=1 -- a directory walk -- only the lowest-numbered piece
# starts a join: any other piece prints nothing and returns 1, so each archive
# is opened exactly once. Named on its own, any piece brings in the whole set.
# A gap in the numbering returns 3: an error, not a silently short archive.
join_pieces() {
	local piece="$1" out_dir="$2" only_first="$3"
	local info stem dir sibling sibling_info number expected="" path
	local -a ordered=()
	info="$(piece_of "$piece")" || return 1
	stem="${info%%$'\t'*}"
	dir="$(cd "$(dirname "$piece")" && pwd)"

	while IFS=$'\t' read -r number path; do
		if [ -z "$expected" ]; then
			if [ "$only_first" = "1" ] && [ "$path" != "$dir/$(basename "$piece")" ]; then
				return 1
			fi
			expected="$number"
		fi
		if [ "$number" != "$expected" ]; then
			echo "Error: $stem has no piece $expected; $(basename "$path") comes next." >&2
			return 3
		fi
		ordered+=("$path")
		expected=$((number + 1))
	done < <(
		for sibling in "$dir"/*; do
			[ -f "$sibling" ] || continue
			sibling_info="$(piece_of "$sibling")" || continue
			[ "${sibling_info%%$'\t'*}" = "$stem" ] || continue
			same_style "$piece" "$sibling" || continue
			printf '%s\t%s\n' "${sibling_info##*$'\t'}" "$sibling"
		done | sort -n -k1,1
	)
	[ "${#ordered[@]}" -gt 0 ] || return 1

	mkdir -p "$out_dir"
	cat "${ordered[@]}" > "$out_dir/$stem"
	{
		printf 'Joined %s from %s piece(s):\n' "$stem" "${#ordered[@]}"
		for path in "${ordered[@]}"; do
			printf '  %s (%s bytes)\n' "$(basename "$path")" "$(wc -c < "$path" | tr -d ' ')"
		done
	} >&2
	printf '%s\n' "$out_dir/$stem"
}

CANDIDATE=""
if [ -f "$SOURCE" ] && [ "$(basename "$SOURCE" | tr '[:upper:]' '[:lower:]')" = "v1.upt" ]; then
	CANDIDATE="$SOURCE"
elif [ -d "$SOURCE" ]; then
	CANDIDATE="$(find_named_upt "$SOURCE")"
fi

# TempoTec V1.1 was distributed as an archive. Extract each top-level file to
# its own directory so equal names in unrelated files cannot overwrite one
# another. A raw UPT also opens in 7-Zip, but produces no nested v1.upt and is
# recognized by is_v1_upt below. The walk is sorted, so the same folder always
# gives the same answer.
if [ -z "$CANDIDATE" ]; then
	ONLY_FIRST=0
	[ -d "$SOURCE" ] && ONLY_FIRST=1
	index=0
	while IFS= read -r file; do
		index=$((index + 1))
		joined=""
		if piece_of "$file" >/dev/null; then
			if joined="$(join_pieces "$file" "$WORK/joined-$index" "$ONLY_FIRST")"; then
				file="$joined"
			else
				status=$?
				[ "$status" -eq 1 ] && continue
				exit 1
			fi
		fi
		if is_v1_upt "$file"; then
			CANDIDATE="$file"
			break
		fi
		out="$WORK/extracted-$index"
		mkdir -p "$out"
		if 7z x "$file" -o"$out" -y >/dev/null 2>&1; then
			CANDIDATE="$(find_named_upt "$out")"
			[ -z "$CANDIDATE" ] || break
		elif [ -n "$joined" ]; then
			echo "Warning: $(basename "$joined"), joined from pieces, did not open; is a piece missing or truncated?" >&2
		fi
	done < <(if [ -d "$SOURCE" ]; then find "$SOURCE" -maxdepth 2 -type f -print | LC_ALL=C sort; else printf '%s\n' "$SOURCE"; fi)
fi

[ -n "$CANDIDATE" ] || {
	echo "Error: no v1.upt was found in $SOURCE." >&2
	echo "Download the official TempoTec Variations V1 firmware, not a driver or instruction PDF." >&2
	exit 1
}
is_v1_upt "$CANDIDATE" || {
	echo "Error: $CANDIDATE has no ota_v0/ota_update.in and is not a supported V1 firmware image." >&2
	exit 1
}

cp -f "$CANDIDATE" "$PACKER_DIR/v1_original.upt"
printf 'Prepared %s (%s bytes)\n' "$PACKER_DIR/v1_original.upt" "$(wc -c < "$PACKER_DIR/v1_original.upt" | tr -d ' ')"
if command -v sha256sum >/dev/null 2>&1; then
	printf 'Stock v1.upt SHA-256: %s\n' "$(sha256sum "$PACKER_DIR/v1_original.upt" | awk '{print $1}')"
fi

# Keep TempoTec's checksum-file wording when one accompanied the download. The
# packer substitutes the new image's MD5 while preserving everything else.
TEMPLATE=""
CANDIDATE_DIR="$(cd "$(dirname "$CANDIDATE")" && pwd)"
TEMPLATE="$(find "$CANDIDATE_DIR" -maxdepth 1 -type f -iname '*md5*.txt' -print -quit 2>/dev/null)"
if [ -z "$TEMPLATE" ] && [ -d "$SOURCE" ]; then
	TEMPLATE="$(find "$SOURCE" -type f -iname '*md5*.txt' -print -quit 2>/dev/null)"
fi
if [ -n "$TEMPLATE" ]; then
	cp -f "$TEMPLATE" "$PACKER_DIR/v1_md5_original.txt"
	printf 'Prepared checksum template %s\n' "$PACKER_DIR/v1_md5_original.txt"
else
	rm -f "$PACKER_DIR/v1_md5_original.txt"
	printf 'No checksum template found; the packer will write standard md5sum syntax.\n'
fi
