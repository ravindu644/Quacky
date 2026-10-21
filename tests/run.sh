#!/bin/sh
# Quacky golden test runner (08). POSIX sh, no framework, so it runs under both
# Fedora's shell and Android's toybox. QUACKY names the binary (default: the
# native sanitizer build); set it to a cross binary to run the same cases on
# another architecture. Set QK_SKIP_TIMING=1 to skip the timing and fuzz tests
# (they measure wall time, so they only make sense running locally, not over adb).
#
#   ./run.sh                      # native build, everything
#   QUACKY=/data/local/tmp/quacky-aarch64 QK_SKIP_TIMING=1 ./run.sh
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$here" || exit 2
QUACKY=${QUACKY:-../output/quacky-hardened}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

pass=0 fail=0
ok()   { pass=$((pass + 1)); }
bad()  { fail=$((fail + 1)); printf 'FAIL %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2"; }

# A good case: transcript matches, --check is clean, and (if present) the HID
# byte stream matches. The script path is relative and stable, so diagnostics
# that embed it stay reproducible across machines.
for qk in cases/*.qk; do
    name=${qk%.qk}
    out=$TMP/out; : > "$out"      # -o is O_APPEND, so start each case from empty
    if ! "$QUACKY" -d 0 -o "$out" "$qk" 2>"$TMP/err"; then
        bad "$qk (ran with exit $?)" "$(cat "$TMP/err")"; continue
    fi
    if cmp -s "$out" "$name.out"; then ok; else
        bad "$qk transcript" "$(diff "$name.out" "$out" 2>/dev/null || true)"; continue
    fi
    if [ -f "$name.hid" ]; then
        dev=$TMP/dev; : > "$dev"
        "$QUACKY" -d 0 --hid "$dev" "$qk" >/dev/null 2>&1
        if cmp -s "$dev" "$name.hid"; then ok; else bad "$qk hid bytes"; fi
    fi
    if [ -f "$name.fast.hid" ]; then
        dev=$TMP/fdev; : > "$dev"
        "$QUACKY" -d 0 --fast --hid "$dev" "$qk" >/dev/null 2>&1
        if cmp -s "$dev" "$name.fast.hid"; then ok; else bad "$qk --fast hid bytes"; fi
    fi
    if [ -f "$name.ultra.hid" ]; then
        dev=$TMP/udev; : > "$dev"
        "$QUACKY" -d 0 --ultra --hid "$dev" "$qk" >/dev/null 2>&1
        if cmp -s "$dev" "$name.ultra.hid"; then ok; else bad "$qk --ultra hid bytes"; fi
    fi
    # --check parses only and writes nothing
    chk=$TMP/chk; : > "$chk"
    if "$QUACKY" --check "$qk" >"$chk" 2>/dev/null && [ ! -s "$chk" ]; then ok; else bad "$qk --check"; fi
done

# An error case: the first line of the .err is the expected exit code, the rest
# is the expected stderr. errors/ run as a normal (transcript) run; errors_hid/
# run with --hid to a regular file (those errors only fire in the HID backend).
check_error() { # $1 = .qk, $2 = extra args
    qk=$1; extra=$2; name=${qk%.qk}
    want_code=$(head -n1 "$name.err")
    want_msg=$(tail -n +2 "$name.err")
    # shellcheck disable=SC2086
    "$QUACKY" -d 0 $extra "$qk" >"$TMP/o" 2>"$TMP/e"; got=$?
    got_msg=$(cat "$TMP/e")
    if [ "$got" = "$want_code" ] && [ "$got_msg" = "$want_msg" ]; then ok; else
        bad "$qk (exit $got, want $want_code)" "$got_msg"
    fi
    # a syntax error must also --check to 2 with no stdout
    case $name in
        */syn_*)
            c=$TMP/c; : > "$c"
            "$QUACKY" --check "$qk" >"$c" 2>/dev/null; cc=$?
            if [ "$cc" = 2 ] && [ ! -s "$c" ]; then ok; else bad "$qk --check (exit $cc)"; fi
            ;;
    esac
}
for qk in cases/errors/*.qk;     do check_error "$qk" "-o $TMP/o"; done
for qk in cases/errors_hid/*.qk; do dev=$TMP/hdev; : > "$dev"; check_error "$qk" "--hid $dev"; done

# The fastmode/ultramode statements must produce exactly the HID bytes of the
# --fast/--ultra flags (and fastmode 0 the bytes of neither). Compare the stream
# from a script that sets the mode against one that types the same with the flag.
printf 'typeln abcabc\n' > "$TMP/mp.qk"
mode_eq() { # $1 label, $2 .qk with the statement, $3 flags for the plain script
    a=$TMP/ma; b=$TMP/mb; : > "$a"; : > "$b"
    # shellcheck disable=SC2086
    if "$QUACKY" -d 0 --hid "$a" "$2" && "$QUACKY" -d 0 $3 --hid "$b" "$TMP/mp.qk" &&
       cmp -s "$a" "$b"; then ok; else bad "$1 (statement != flag)"; fi
}
printf 'ultramode 1\ntypeln abcabc\n' > "$TMP/mu.qk"; mode_eq "ultramode==--ultra" "$TMP/mu.qk" "--ultra"
printf 'fastmode 1\ntypeln abcabc\n'  > "$TMP/mf.qk"; mode_eq "fastmode==--fast"   "$TMP/mf.qk" "--fast"
printf 'fastmode 0\ntypeln abcabc\n'  > "$TMP/mo.qk"; mode_eq "fastmode0==plain"   "$TMP/mo.qk" ""

# Timing and fuzzing: wall-clock sensitive, so local runs only. Measure against
# an -O2 build, not the ASan one whose start-up cost would blur the tight bounds;
# the x86_64 release binary runs on this host when `make all-build` has produced it.
if [ "${QK_SKIP_TIMING:-0}" != 1 ]; then
    TQ=$QUACKY
    if [ "$(uname -m)" = x86_64 ] && [ -x ../output/quacky-x86_64 ]; then
        TQ=../output/quacky-x86_64
    fi
    # 100 characters at 20 ms = 100 ticks = 2000 ms (first char immediate).
    printf 'typeln %s\n' "$(printf 'a%.0s' $(seq 1 100))" > "$TMP/t100.qk"
    t0=$(date +%s%N); "$TQ" -d 20 -o /dev/null "$TMP/t100.qk"; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -ge 1990 ] && [ "$ms" -le 2150 ]; then ok; else bad "timing 100ch (${ms}ms, want 1990-2150)"; fi

    # sleep 500 then one key must land at ~500 ms, not 500 + a tick. The \c drops
    # the trailing newline so only the single key is timed; the upper bound leaves
    # room for process start-up under ASan but still excludes a full extra tick.
    printf 'sleep 500\ntype x\\c\n' > "$TMP/ts.qk"
    t0=$(date +%s%N); "$TQ" -d 20 -o /dev/null "$TMP/ts.qk"; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -ge 480 ] && [ "$ms" -le 520 ]; then ok; else bad "timing sleep (${ms}ms, want 480-520)"; fi

    # Drift: 500 characters at 10 ms must finish inside 5000 + 60 ms.
    printf 'typeln %s\n' "$(printf 'a%.0s' $(seq 1 500))" > "$TMP/t500.qk"
    t0=$(date +%s%N); "$TQ" -d 10 -o /dev/null "$TMP/t500.qk"; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -le 5060 ] && [ "$ms" -ge 4990 ]; then ok; else bad "timing drift (${ms}ms, want 4990-5060)"; fi

    # delay paces only output actions, not logic. With delay 200, a 50-iteration
    # loop of pure assignments plus one type must finish fast: the type is the
    # first action so it owes no gap, and nothing else pauses. The old
    # after-every-statement bug made this 50+ pauses, i.e. 10+ seconds.
    printf 'delay 200\ni = 0\nwhile i < 50 do\n  i = i + 1\ndone\ntype done\n' > "$TMP/dn.qk"
    t0=$(date +%s%N); "$TQ" -d 0 -o /dev/null "$TMP/dn.qk"; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -le 150 ]; then ok; else bad "timing delay-on-logic (${ms}ms, want <=150)"; fi

    # delay 200 between three typed lines is two gaps (before the 2nd and 3rd,
    # none before the 1st or after the 3rd) = 400 ms.
    printf 'delay 200\ntypeln a\ntypeln b\ntypeln c\n' > "$TMP/dg.qk"
    t0=$(date +%s%N); "$TQ" -d 0 -o /dev/null "$TMP/dg.qk"; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -ge 380 ] && [ "$ms" -le 520 ]; then ok; else bad "timing delay-gap (${ms}ms, want 380-520)"; fi

    # An explicit sleep adds to the default delay, it does not replace it:
    # type a (first, no gap), sleep 100, then the 200 gap before type b = 300 ms.
    printf 'delay 200\ntype a\nsleep 100\ntype b\n' > "$TMP/da.qk"
    t0=$(date +%s%N); "$TQ" -d 0 -o /dev/null "$TMP/da.qk"; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -ge 280 ] && [ "$ms" -le 400 ]; then ok; else bad "timing delay-additive (${ms}ms, want 280-400)"; fi

    # keydelay is the per-key tick from inside the script (an in-script -d). At
    # -d 0, typing 11 characters (10 + newline) at keydelay 40 is 10 gaps (the
    # first key is immediate) = 400 ms.
    printf 'keydelay 40\ntypeln aaaaaaaaaa\n' > "$TMP/kd.qk"
    t0=$(date +%s%N); "$TQ" -d 0 -o /dev/null "$TMP/kd.qk"; t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if [ "$ms" -ge 380 ] && [ "$ms" -le 520 ]; then ok; else bad "timing keydelay (${ms}ms, want 380-520)"; fi

    # Byte-mutation fuzz: a mutated script must exit 0, 1 or 2, never crash (>=128).
    crash=0
    for qk in cases/*.qk; do
        i=0
        while [ "$i" -lt 20 ]; do
            perl -e 'local $/; my $d=<>; my $p=int(rand(length $d)); substr($d,$p,1)=chr(int(rand(256))); print $d' "$qk" > "$TMP/fz.qk" 2>/dev/null
            timeout 2 "$QUACKY" -d 0 -o /dev/null "$TMP/fz.qk" >/dev/null 2>&1; rc=$?
            if [ "$rc" -ge 128 ]; then crash=1; printf 'FUZZ CRASH exit %s on mutated %s\n' "$rc" "$qk"; fi
            i=$((i + 1))
        done
    done
    [ "$crash" = 0 ] && ok || bad "fuzz (a mutation crashed)"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = 0 ]
