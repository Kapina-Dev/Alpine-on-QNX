# JIT smoke app: LuaJIT, run by scripts/run-jit-smoke.sh (not part of the
# corpus). The JIT writes ARM code into anonymous memory and flips it
# executable with mprotect; its output must match the interpreter's.
corpus_packages="luajit"

luajit_workload()
{
    cat > "$(rootfs_path /tmp/corpus-jit.lua)" <<'LUA'
local n = tonumber(arg and arg[1]) or 1000000
local s = 0
for i = 1, n do s = (s + i * 7 % 13) % 1000003 end
local t = {}
for i = 1, 100000 do t[i] = i * 3 end
local u = 0
for i = 1, #t do u = u + t[i] end
local parts = {}
for i = 1, 20000 do parts[#parts + 1] = tostring(i % 97) end
local str = table.concat(parts, ",")
local f = 0.0
for i = 1, n do f = f + i * 0.5 / (i % 7 + 1) end
print(s, u, #str, string.format("%.3f", f))
LUA
}

stage_startup()
{
    out=$(guest 30 /usr/bin/luajit -v 2>&1)
    case $out in "LuaJIT 2.1"*) ;; *) echo "unexpected: $out"; return 1 ;; esac
    out=$(guest 30 /usr/bin/luajit -e 'print(jit.status())' 2>&1)
    case $out in true*) ;; *) echo "JIT not enabled: $out"; return 1 ;; esac
}

stage_basic()
{
    luajit_workload
    off=$(guest 300 /usr/bin/luajit -joff /tmp/corpus-jit.lua) ||
        { echo "interpreter run failed"; return 1; }
    on=$(guest 300 /usr/bin/luajit /tmp/corpus-jit.lua) ||
        { echo "JIT run failed"; return 1; }
    [ -n "$off" ] && [ "$on" = "$off" ] ||
        { echo "JIT output '$on' differs from interpreter '$off'"; return 1; }
    echo "$off" > "$corpus_app_out/expected.txt"
    traces=$(guest 300 /usr/bin/luajit -jv /tmp/corpus-jit.lua 2>&1 >/dev/null |
        grep -c '^\[TRACE')
    [ "${traces:-0}" -gt 0 ] || { echo "no traces compiled"; return 1; }
}

stage_concurrent()
{
    expected=$(cat "$corpus_app_out/expected.txt")
    out=$(guest_sh 300 'for i in 1 2 3 4; do
            luajit /tmp/corpus-jit.lua > /tmp/corpus-jit.$i &
        done; wait; cat /tmp/corpus-jit.1 /tmp/corpus-jit.2 \
            /tmp/corpus-jit.3 /tmp/corpus-jit.4')
    lines=$(printf '%s\n' "$out" | grep -c -x -F "$expected")
    [ "$lines" = 4 ] || { echo "parallel JIT runs: $out"; return 1; }
}

stage_shutdown()
{
    # A clean exit from JIT-compiled code keeps its status.
    guest 60 /usr/bin/luajit -e \
        'local s = 0 for i = 1, 1000000 do s = s + i end os.exit(s > 0 and 3 or 4)'
    status=$?
    rm -f "$(rootfs_path /tmp/corpus-jit.lua)" "$(rootfs_path /tmp)"/corpus-jit.?
    [ "$status" = 3 ] || { echo "exit status $status, expected 3"; return 1; }
}
