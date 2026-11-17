# Corpus app: Vim.
corpus_packages="vim"

stage_startup()
{
    out=$(TERM=dumb guest 30 /usr/bin/vim --version 2>&1 | head -n 1)
    case $out in "VIM - Vi IMproved 9.2"*) ;;
        *) echo "unexpected: $out"; return 1 ;; esac
}

stage_basic()
{
    out=$(guest_sh 60 'export TERM=dumb HOME=/tmp; rm -rf /tmp/corpus-vim
        mkdir /tmp/corpus-vim; cd /tmp/corpus-vim; echo aaa > f
        vim -es -c "%s/a/b/g" -c wq f; cat f')
    [ "$out" = bbb ] || { echo "after edit: $out"; return 1; }
}

stage_concurrent()
{
    out=$(guest_sh 120 'export TERM=dumb HOME=/tmp; cd /tmp/corpus-vim
        for i in 1 2 3 4; do echo "x$i x$i" > g$i; done
        for i in 1 2 3 4; do vim -es -c "%s/x/y/g" -c wq g$i & done; wait
        cat g1 g2 g3 g4' | tr '\n' ' ')
    [ "$out" = "y1 y1 y2 y2 y3 y3 y4 y4 " ] ||
        { echo "after 4 parallel edits: $out"; return 1; }
}

stage_shutdown()
{
    left=$(ls -a "$(rootfs_path /tmp/corpus-vim)" | grep '\.sw[a-p]$')
    [ -z "$left" ] || { echo "swap files left: $left"; return 1; }
}
