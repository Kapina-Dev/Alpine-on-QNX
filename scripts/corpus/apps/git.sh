# Corpus app: Git.
corpus_packages="git"

stage_startup()
{
    out=$(guest 30 /usr/bin/git --version 2>&1)
    echo "$out" | grep -q '^git version 2\.' ||
        { echo "unexpected: $out"; return 1; }
}

stage_basic()
{
    out=$(guest_sh 120 'set -e; export HOME=/tmp; rm -rf /tmp/corpus-git
        mkdir /tmp/corpus-git; cd /tmp/corpus-git; git init -q
        git config user.email corpus@example.invalid
        git config user.name corpus; echo hi > f; git add f
        git commit -qm one; git log --oneline | wc -l')
    [ "$out" = 1 ] || { echo "git log count: $out"; return 1; }
}

stage_concurrent()
{
    out=$(guest_sh 180 'export HOME=/tmp; cd /tmp/corpus-git
        rm -f /tmp/corpus-git-ok*; for i in 1 2 3 4; do
            (git log --oneline >/dev/null && echo ok > /tmp/corpus-git-ok$i) &
        done; git gc --quiet; s=$?; wait; echo $s
        ls /tmp/corpus-git-ok* | wc -l' | tr '\n' ' ')
    [ "$out" = "0 4 " ] || { echo "gc status and logs ok: $out"; return 1; }
}

stage_shutdown()
{
    for lock in /tmp/corpus-git/.git/index.lock /tmp/corpus-git/.git/gc.pid; do
        [ ! -e "$(rootfs_path "$lock")" ] || { echo "left behind: $lock"; return 1; }
    done
}
