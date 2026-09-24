#!/bin/sh
set -eu

branch=${1:-HEAD}
maximum_blob_bytes=${MAX_PUBLIC_BLOB_BYTES:-5242880}
tmp_base=${TMPDIR:-/tmp}/linuxemu-publication-audit.$$
objects=$tmp_base.objects
paths=$tmp_base.paths
content=$tmp_base.content

cleanup()
{
    rm -f "$objects" "$paths" "$content"
}
trap cleanup EXIT HUP INT TERM

git rev-parse --verify "$branch^{commit}" >/dev/null
git rev-list --objects "$branch" >"$objects"
awk 'NF > 1 { $1=""; sub(/^ /, ""); print }' "$objects" >"$paths"

forbidden_paths='(^|/)(bb10-root-artifacts|bb10-results|bb10-contract)-|(^|/)analysis-tools/|(^|/)id_rsa($|\.)|\.pyc$|\.exe$|(^|/)gcc-sdk\.zip$'
if grep -E "$forbidden_paths" "$paths"; then
    echo "publication audit: forbidden path found in $branch history" >&2
    exit 1
fi

git log "$branch" -p --no-ext-diff >"$content"
credential_markers='BEGIN (RSA |OPENSSH |EC |DSA )?PRIVATE KEY|gh[pousr]_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}'
if grep -En "$credential_markers" "$content"; then
    echo "publication audit: credential-like content found in $branch history" >&2
    exit 1
fi

large=0
while read -r object path; do
    [ -n "${path:-}" ] || continue
    [ "$(git cat-file -t "$object")" = blob ] || continue
    size=$(git cat-file -s "$object")
    if [ "$size" -gt "$maximum_blob_bytes" ]; then
        printf '%s bytes  %s\n' "$size" "$path" >&2
        large=1
    fi
done <"$objects"
[ "$large" -eq 0 ] || {
    echo "publication audit: blob exceeds $maximum_blob_bytes bytes" >&2
    exit 1
}

printf 'publication_branch=%s\n' "$(git rev-parse "$branch^{commit}")"
printf 'publication_commits=%s\n' "$(git rev-list --count "$branch")"
printf 'publication_paths=%s\n' "$(wc -l <"$paths" | awk '{print $1}')"
echo 'publication_forbidden_paths=0'
echo 'publication_credential_markers=0'
echo 'publication_oversized_blobs=0'
