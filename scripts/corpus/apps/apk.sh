# Corpus app: apk-tools. Install reinstalls the pinned closure over the base.
corpus_packages="apk-tools"

stage_startup()
{
    out=$(guest 30 /sbin/apk --version 2>&1)
    echo "$out" | grep -q 'apk-tools 3\.' ||
        { echo "unexpected: $(echo "$out" | head -n 1)"; return 1; }
}

stage_basic()
{
    guest 60 /sbin/apk info -e busybox >/dev/null ||
        { echo "apk info -e busybox failed"; return 1; }
    first=$(awk 'NR == 1 { print $2 }' \
        "$corpus_manifest_dir/$corpus_app-apks.sha256")
    cp "$corpus_manifest_base/$first" "$(rootfs_path /tmp/corpus-verify.apk)"
    guest 120 /sbin/apk verify /tmp/corpus-verify.apk ||
        { echo "apk verify of a pinned package failed"; return 1; }
}

stage_concurrent()
{
    out=$(guest_sh 120 'rm -f /tmp/corpus-apk-ok*; for i in 1 2 3 4; do
            (/sbin/apk info -e busybox >/dev/null &&
                echo ok > /tmp/corpus-apk-ok$i) &
        done; wait; ls /tmp/corpus-apk-ok* | wc -l')
    [ "$out" = 4 ] || { echo "parallel apk info: $out of 4 ok"; return 1; }
}

stage_shutdown()
{
    # No apk process may still hold the database: a new one runs promptly.
    guest 10 /sbin/apk info >/dev/null ||
        { echo "apk info did not finish within 10s"; return 1; }
}
