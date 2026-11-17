#!/usr/bin/env bash
# Runs the host tests of this repository the way the README ("Native
# validation") describes them, every command under a time limit, and writes
# a table of the results to the job summary ($GITHUB_STEP_SUMMARY).
#
# usage: run-host-tests.sh <exteplayer3 tree> <work directory> [title]
# needs: FFMPEG_SOURCE, FFMPEG_BUILD (FFmpeg configured and built natively,
#        static), FFMPEG_EXTRA_LIBS (the external libraries of that build)
#
# The tests are used as they are in the tree. Nothing here changes them; a
# test that is not in the tree is reported as "not run".
# Exit status 1 when at least one command failed.

set -u
: "${FFMPEG_SOURCE:?}" "${FFMPEG_BUILD:?}"
UT=$(cd "$1" && pwd)
mkdir -p "$2/logs" "$2/out"
W=$(cd "$2" && pwd)
TITLE=${3:-host tests}
OUT=$W/out
FF=$FFMPEG_BUILD/ffmpeg
DRIVER=$FFMPEG_BUILD/libavformat/tests/hls_http
FIFO=$OUT/audio.fifo
APES=$OUT/audio.pes
VPES=$OUT/video.pes
RESULTS=$W/results.tsv
: > "$RESULTS"
failed=0

note() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$RESULTS"; }

# step <id> <seconds> <what it covers> <command...>
# Result in $rc. Leftovers of an earlier, aborted test (FIFO, captures) are
# removed first: the tests insist on paths that do not exist yet.
step() {
  local id=$1 tmo=$2 what=$3 start=$SECONDS state
  shift 3
  rm -f "$FIFO" "$APES" "$VPES"
  echo "::group::$id"
  echo "+ $*"
  (cd "$UT" && timeout -k 10 "$tmo" "$@") 2>&1 | tee "$W/logs/$id.log"
  rc=${PIPESTATUS[0]}
  echo "::endgroup::"
  if [ "$rc" = 0 ]; then
    state=pass
  elif [ "$rc" = 124 ] || [ "$rc" = 137 ]; then
    state="TIMEOUT after $tmo s"
  else
    state="FAIL (exit $rc)"
  fi
  if [ "$rc" != 0 ]; then
    failed=1
    echo "::error::$TITLE: $id: $state"
  fi
  note "$id" "$state" "$((SECONDS - start)) s" "$what"
}

skip() { # skip <id> <reason> <what it covers>
  echo "::warning::$TITLE: $1 not run: $2"
  note "$1" "not run: $2" "" "$3"
}

have() { [ -f "$UT/$1" ]; }

# ---- tests/run.sh: the small test programs (ASan / UBSan) ----------------
if have tests/run.sh; then
  # -x only to see in the log which program was compiled and started
  step run.sh 600 "tests/run.sh: all unit test programs" sh -x tests/run.sh
  # one line per program of its main loop: compiled, started, failed?
  log=$W/logs/run.sh.log
  wanted=$(sed -n 's/^for test in \(.*\); do$/\1/p' "$UT/tests/run.sh" | head -1)
  started=$(sed -n 's/^+ timeout [0-9]* .*\/\([a-z_0-9]*\)$/\1/p' "$log")
  last=$(echo "$started" | tail -1)
  for t in $wanted; do
    if ! grep -q -- "-o /[^ ]*/$t\$" "$log"; then
      state="not reached"
    elif ! echo "$started" | grep -qx "$t"; then
      state="FAIL (did not compile)"
    elif [ "$rc" != 0 ] && [ "$t" = "$last" ]; then
      state="FAIL (exit $rc)"
    else
      state=pass
    fi
    note "  ${t}_test" "$state" "" "tests/${t}_test.c"
  done
else
  skip run.sh "tests/run.sh is not in this tree" "unit test programs"
fi

# ---- tests/build.sh: the host player, four variants ----------------------
PCM="-DPCM_EXPORT_PATH=\"$FIFO\""
host=no; capture=no; timing=no; asan=no
if have tests/build.sh; then
  step build-host 600 "host player with an isolated PCM FIFO (+ HTTP seek helper)" \
    env "CFLAGS=$PCM" sh tests/build.sh "$OUT/exteplayer3-host"
  [ "$rc" = 0 ] && host=yes
  step build-capture 600 "host player capturing audio PES, HLS format alias" \
    env "CFLAGS=$PCM" "HOST_AUDIO_OUTPUT=$APES" HOST_HLS_ALIAS=1 \
    sh tests/build.sh "$OUT/exteplayer3-capture"
  [ "$rc" = 0 ] && capture=yes
  step build-timing 600 "host player capturing audio and video PES, HLS format alias" \
    env "CFLAGS=$PCM" "HOST_AUDIO_OUTPUT=$APES" "HOST_VIDEO_OUTPUT=$VPES" HOST_HLS_ALIAS=1 \
    sh tests/build.sh "$OUT/exteplayer3-timing"
  [ "$rc" = 0 ] && timing=yes
  step build-asan 600 "host player with ASan + UBSan" \
    env "CFLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer $PCM" \
    sh tests/build.sh "$OUT/exteplayer3-asan"
  [ "$rc" = 0 ] && asan=yes
else
  skip build.sh "tests/build.sh is not in this tree" "host player"
fi

# ---- tests/integration.py -------------------------------------------------
what="HTTP TS seek, HLS quality, single-file DASH audio switch, subtitles, radio metadata, PCM FIFO, CLI validation"
if ! have tests/integration.py; then
  skip integration "tests/integration.py is not in this tree" "$what"
else
  if [ $host = yes ]; then
    step integration 600 "$what" \
      python3 tests/integration.py --player "$OUT/exteplayer3-host" --ffmpeg "$FF" --pcm-fifo "$FIFO"
  else
    skip integration "the host player did not build" "$what"
  fi
  if [ $asan = yes ]; then
    step integration-asan 900 "the same with the ASan + UBSan player" \
      env ASAN_OPTIONS=detect_leaks=0 \
      python3 tests/integration.py --player "$OUT/exteplayer3-asan" --ffmpeg "$FF" --pcm-fifo "$FIFO"
  else
    skip integration-asan "the ASan player did not build" "the same with the ASan + UBSan player"
  fi
fi

# ---- tests/hls_switch.py: the four modes the README lists ----------------
if ! have tests/hls_switch.py; then
  skip hls_switch "tests/hls_switch.py is not in this tree" "HLS audio switching"
elif [ ! -f "$FFMPEG_SOURCE/tests/hls_http.py" ]; then
  skip hls_switch "tests/hls_http.py is not in the FFmpeg tree" "HLS audio switching"
else
  what="HLS audio switching, decoded PCM through the FIFO (VOD + live, sync off/on)"
  if [ $host = yes ]; then
    step hls_switch-export 900 "$what" \
      python3 tests/hls_switch.py --player "$OUT/exteplayer3-host" --ffmpeg "$FF" --pcm-fifo "$FIFO"
  else
    skip hls_switch-export "the host player did not build" "$what"
  fi
  what1="HLS audio switching, compressed AAC from the PES capture, three qualities, format alias"
  what2="HLS audio switching, software decoded PCM from the PES capture"
  if [ $capture = yes ]; then
    step hls_switch-compressed 900 "$what1" \
      python3 tests/hls_switch.py --player "$OUT/exteplayer3-capture" --ffmpeg "$FF" --pcm-fifo "$FIFO" \
      --audio-output "$APES" --mode compressed --multiple --video-first --quality all
    step hls_switch-software 900 "$what2" \
      python3 tests/hls_switch.py --player "$OUT/exteplayer3-capture" --ffmpeg "$FF" --pcm-fifo "$FIFO" \
      --audio-output "$APES" --mode software
  else
    skip hls_switch-compressed "the capture player did not build" "$what1"
    skip hls_switch-software "the capture player did not build" "$what2"
  fi
  what="live HLS: source time against audio/video PES, writer backpressure"
  if [ $timing != yes ]; then
    skip hls_switch-timing "the timing player did not build" "$what"
  elif [ ! -x "$DRIVER" ]; then
    skip hls_switch-timing "libavformat/tests/hls_http is not in the FFmpeg build" "$what"
  else
    step hls_switch-timing 900 "$what" \
      python3 tests/hls_switch.py --player "$OUT/exteplayer3-timing" --ffmpeg "$FF" --pcm-fifo "$FIFO" \
      --audio-output "$APES" --video-output "$VPES" --timing-driver "$DRIVER" \
      --mode compressed --case live --multiple --video-first --quality all --sync default --backpressure
  fi
fi

# ---- summary ---------------------------------------------------------------
{
  echo "### $TITLE: exteplayer3 $(git -C "$UT" rev-parse --short=10 HEAD)"
  echo
  echo "| test | result | time | covers |"
  echo "|---|---|---|---|"
  while IFS=$'\t' read -r id state time what; do
    case "$id" in "  "*) id="&nbsp;&nbsp;&nbsp;${id#  }";; *) id="**$id**";; esac
    echo "| $id | $state | $time | $what |"
  done < "$RESULTS"
  echo
  if [ $failed = 0 ]; then
    echo "All commands that ran passed."
  else
    echo "At least one command failed; the last lines of each failing log:"
    while IFS=$'\t' read -r id state _ _; do
      case "$id" in "  "*) continue;; esac
      case "$state" in pass|"not run"*) continue;; esac
      echo
      echo "<details><summary>$id: $state</summary>"
      echo
      echo '```'
      tail -40 "$W/logs/$id.log" | cut -c1-600
      echo '```'
      echo "</details>"
    done < "$RESULTS"
  fi
} > "$W/summary.md"
cat "$W/summary.md"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then cat "$W/summary.md" >> "$GITHUB_STEP_SUMMARY"; fi
exit $failed
