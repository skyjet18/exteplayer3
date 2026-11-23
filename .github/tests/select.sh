# Sourced by run-host-tests.sh and run-ffmpeg-tests.sh: tests selected by hand.
#
# TESTS (workflow input "tests"): comma separated step ids, e.g.
# "integration,hls_switch-timing". An id also selects the steps whose id is
# that id followed by "-" and more ("hls_switch" selects hls_switch-export,
# hls_switch-timing, ...; "integration" also integration-asan). "ffmpeg"
# selects all companion tests of the FFmpeg tree. Empty (pushes, pull
# requests): every test, as before.
# TESTS_RAN (optional): file that gets the id of every test step that runs,
# so the workflow can tell a selection that matched nothing.

TESTS=${TESTS:-}
TESTS=${TESTS// /}

selected() { # selected <step id> [group]
  [ -z "$TESTS" ] && return 0
  case ",$TESTS," in
    *",$1,"* | *",${1%%-*},"*) return 0 ;;
  esac
  if [ -n "${2:-}" ]; then
    case ",$TESTS," in *",$2,"*) return 0 ;; esac
  fi
  return 1
}

wants() { # wants <step id...>: one of them is selected
  local id
  for id; do selected "$id" && return 0; done
  return 1
}

test_ran() { # test_ran <step id>
  if [ -n "${TESTS_RAN:-}" ]; then echo "$1" >> "$TESTS_RAN"; fi
  return 0
}
