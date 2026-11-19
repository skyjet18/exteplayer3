#!/usr/bin/env bash
# Runs the tests of the FFmpeg tree that the README ("Native validation")
# lists next to the player tests, each under a time limit, and writes a
# table of the results to the job summary ($GITHUB_STEP_SUMMARY).
#
# usage: run-ffmpeg-tests.sh <log directory> [title]
# needs: FFMPEG_SOURCE, FFMPEG_BUILD (FFmpeg configured and built natively,
#        with the test programs libavformat/tests/hls_http and dash_http)
# Exit status 1 when at least one test failed.

set -u
: "${FFMPEG_SOURCE:?}" "${FFMPEG_BUILD:?}"
mkdir -p "$1"
LOGS=$(cd "$1" && pwd)
TITLE=${2:-FFmpeg}
failed=0
rows=""

step() { # step <id> <seconds> <command...>
  local id=$1 tmo=$2 start=$SECONDS state rc
  shift 2
  echo "::group::$id"
  echo "+ $*"
  (cd "$FFMPEG_BUILD" && timeout -k 10 "$tmo" "$@") 2>&1 | tee "$LOGS/$id.log"
  rc=${PIPESTATUS[0]}
  echo "::endgroup::"
  if [ "$rc" = 0 ]; then state=pass
  elif [ "$rc" = 124 ] || [ "$rc" = 137 ]; then state="TIMEOUT after $tmo s"
  else state="FAIL (exit $rc)"; fi
  if [ "$rc" != 0 ]; then
    failed=1
    echo "::error::$TITLE: $id: $state"
    tail -40 "$LOGS/$id.log"
  fi
  rows="$rows| $id | $state | $((SECONDS - start)) s |"$'\n'
}

for t in fate-hls-selection fate-mpegts-probe fate-dash-live; do
  step "$t" 900 make "$t"
done
step dash_http.py 900 python3 "$FFMPEG_SOURCE/tests/dash_http.py" \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" --driver "$FFMPEG_BUILD/libavformat/tests/dash_http"
step hls_http.py 900 python3 "$FFMPEG_SOURCE/tests/hls_http.py" \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" --driver "$FFMPEG_BUILD/libavformat/tests/hls_http"
step hls_http.py-realtime 900 python3 "$FFMPEG_SOURCE/tests/hls_http.py" \
  --ffmpeg "$FFMPEG_BUILD/ffmpeg" --driver "$FFMPEG_BUILD/libavformat/tests/hls_http" --realtime

{
  echo "### $TITLE: companion tests of the FFmpeg tree"
  echo
  echo "| test | result | time |"
  echo "|---|---|---|"
  printf '%s' "$rows"
  echo
} | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
exit $failed
