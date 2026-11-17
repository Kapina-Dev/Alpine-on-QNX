# Corpus app: GCC and make.
corpus_packages="gcc make musl-dev"

stage_startup()
{
    out=$(guest 60 /usr/bin/gcc --version 2>&1 | head -n 1)
    case $out in *15.2*) ;; *) echo "unexpected: $out"; return 1 ;; esac
}

stage_basic()
{
    dir=$(rootfs_path /tmp/corpus-cc)
    rm -rf "$dir"
    mkdir -p "$dir"
    printf '#include <stdio.h>\nint main(void) { puts("hello"); return 0; }\n' \
        > "$dir/hello.c"
    out=$(guest_sh 300 'cd /tmp/corpus-cc && gcc -o hello hello.c && ./hello')
    [ "$out" = hello ] || { echo "hello.c: $out"; return 1; }
}

stage_concurrent()
{
    dir=$(rootfs_path /tmp/corpus-cc)
    for part in a b c d; do
        printf 'int f_%s(void) { return 1; }\n' "$part" > "$dir/$part.c"
    done
    printf '#include <stdio.h>\nint f_a(void); int f_b(void); int f_c(void); int f_d(void);\nint main(void) { printf("%%d\\n", f_a() + f_b() + f_c() + f_d()); return 0; }\n' \
        > "$dir/main.c"
    printf 'prog: main.o a.o b.o c.o d.o\n\tgcc -o $@ $^\n%%.o: %%.c\n\tgcc -c -o $@ $<\n' \
        > "$dir/Makefile"
    # Below the 300 s stage timeout, so the guest's own limit fires first.
    out=$(guest_sh 280 'cd /tmp/corpus-cc && make -j4 -s && ./prog')
    [ "$out" = 4 ] || { echo "make -j4 result: $out"; return 1; }
}

stage_shutdown()
{
    left=$(ls "$(rootfs_path /tmp)" | grep '^cc' || true)
    [ -z "$left" ] || { echo "compiler temporaries left: $left"; return 1; }
}
