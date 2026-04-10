#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# compile_all.sh  —  Compile every pattern_*.cpp in this directory
#
# Usage:
#   ./compile_all.sh          # compile only (no run)
#   ./compile_all.sh --run    # compile then run smoke tests
#   ./compile_all.sh --jobs 4 # parallel compile with 4 workers (default: 4)
#   ./compile_all.sh --clean  # remove bin/ and exit
#
# Output binaries go to ./bin/
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────────
CXX="${CXX:-g++}"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Wshadow"
BINDIR="bin"
JOBS=4
RUN=0

# ── Colors ────────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then          # only color when stdout is a terminal
  GRN='\033[0;32m'; RED='\033[0;31m'; YEL='\033[0;33m'
  CYN='\033[0;36m'; BLD='\033[1m';    RST='\033[0m'
else
  GRN=''; RED=''; YEL=''; CYN=''; BLD=''; RST=''
fi

# ── Argument parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case $1 in
    --run)   RUN=1 ;;
    --clean) rm -rf "$BINDIR"; echo "Removed $BINDIR/"; exit 0 ;;
    --jobs)  JOBS="$2"; shift ;;
    --cxx)   CXX="$2"; shift ;;
    -h|--help)
      sed -n '3,12p' "$0" | sed 's/^# \?//'
      exit 0 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
  shift
done

# ── Sanity checks ─────────────────────────────────────────────────────────────
if ! command -v "$CXX" &>/dev/null; then
  echo -e "${RED}ERROR:${RST} compiler '${CXX}' not found in PATH"
  exit 1
fi

SRCS=( pattern_*.cpp )
if [[ ${#SRCS[@]} -eq 0 ]]; then
  echo "No pattern_*.cpp files found in $(pwd)"
  exit 1
fi

mkdir -p "$BINDIR"

# ── Compile function (runs in subshell for parallel) ──────────────────────────
compile_one() {
  local src="$1"
  local name="${src%.cpp}"           # strip .cpp
  local out="${BINDIR}/${name}"
  local log="${BINDIR}/${name}.log"

  if "$CXX" $CXXFLAGS "$src" -o "$out" 2>"$log"; then
    rm -f "$log"
    echo "OK ${src}"
  else
    echo "FAIL ${src}"
  fi
}
export -f compile_one
export CXX CXXFLAGS BINDIR

# ── Header ────────────────────────────────────────────────────────────────────
echo -e "${BLD}${CYN}Compiling ${#SRCS[@]} pattern files  [${CXX} ${CXXFLAGS}]${RST}"
echo -e "${CYN}Output → ${BINDIR}/${RST}"
echo "────────────────────────────────────────────────────────────────"

START=$(date +%s%N 2>/dev/null || date +%s)

# ── Parallel compilation using background jobs ────────────────────────────────
declare -a PIDS=()
declare -a SRC_MAP=()
declare -a RESULT=()

run_with_limit() {
  # When we already have $JOBS running, wait for one to finish
  while (( ${#PIDS[@]} >= JOBS )); do
    local i
    for i in "${!PIDS[@]}"; do
      if ! kill -0 "${PIDS[$i]}" 2>/dev/null; then
        unset "PIDS[$i]"
        PIDS=("${PIDS[@]}")   # re-index
        break
      fi
    done
    sleep 0.05
  done
}

# Launch each compile job
declare -A JOB_RESULT   # src -> "OK"|"FAIL"
for src in "${SRCS[@]}"; do
  run_with_limit
  (
    name="${src%.cpp}"
    out="${BINDIR}/${name}"
    log="${BINDIR}/${name}.log"
    if "$CXX" $CXXFLAGS "$src" -o "$out" 2>"$log"; then
      rm -f "$log"
      printf "OK\t%s\n" "$src" >> "${BINDIR}/.results"
    else
      printf "FAIL\t%s\n" "$src" >> "${BINDIR}/.results"
    fi
  ) &
  PIDS+=($!)
done

# Wait for all remaining jobs
wait "${PIDS[@]}" 2>/dev/null || true

# ── Collect and display results ───────────────────────────────────────────────
PASS=0; FAIL=0
declare -a FAILED_SRCS=()

# Sort results by filename for deterministic output
if [[ -f "${BINDIR}/.results" ]]; then
  while IFS=$'\t' read -r status src; do
    name="${src%.cpp}"
    printf "  %-50s" "$src"
    if [[ "$status" == "OK" ]]; then
      echo -e "${GRN}[  OK  ]${RST}"
      (( PASS++ )) || true
    else
      echo -e "${RED}[ FAIL ]${RST}"
      (( FAIL++ )) || true
      FAILED_SRCS+=("$src")
    fi
  done < <(sort "${BINDIR}/.results")
  rm -f "${BINDIR}/.results"
fi

END=$(date +%s%N 2>/dev/null || date +%s)
# Compute elapsed (handle both ns and s precision)
if [[ ${#START} -gt 10 ]]; then
  ELAPSED=$(( (END - START) / 1000000 ))
  TIME_STR="${ELAPSED}ms"
else
  ELAPSED=$(( END - START ))
  TIME_STR="${ELAPSED}s"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo "────────────────────────────────────────────────────────────────"
echo -e "  ${BLD}Results:${RST}  ${GRN}${PASS} passed${RST}  /  ${RED}${FAIL} failed${RST}  /  ${#SRCS[@]} total   (${TIME_STR})"

if (( FAIL > 0 )); then
  echo ""
  echo -e "${YEL}Failed files — see .log files in ${BINDIR}/ for details:${RST}"
  for f in "${FAILED_SRCS[@]}"; do
    log="${BINDIR}/${f%.cpp}.log"
    echo -e "  ${RED}✗${RST} ${f}"
    if [[ -f "$log" ]]; then
      # Print first 5 lines of error
      head -5 "$log" | sed 's/^/      /'
    fi
  done
fi

echo ""

# ── Optional: run smoke tests ─────────────────────────────────────────────────
if (( RUN == 1 && PASS > 0 )); then
  echo -e "${BLD}${CYN}Running smoke tests...${RST}"
  echo "════════════════════════════════════════════════════════════════"
  RTOTAL=0; RPASS=0; RFAIL=0
  for src in "${SRCS[@]}"; do
    name="${src%.cpp}"
    bin="${BINDIR}/${name}"
    # Windows: binary may have .exe extension added by MinGW
    [[ ! -x "$bin" && -x "${bin}.exe" ]] && bin="${bin}.exe"
    [[ ! -x "$bin" ]] && continue
    (( RTOTAL++ )) || true
    echo -e "${BLD}▶ ${name}${RST}"
    if timeout 10 "$bin" 2>&1 | head -40; then
      (( RPASS++ )) || true
    else
      echo -e "${RED}  (non-zero exit or timeout)${RST}"
      (( RFAIL++ )) || true
    fi
    echo ""
  done
  echo "════════════════════════════════════════════════════════════════"
  echo -e "  ${BLD}Run results:${RST}  ${GRN}${RPASS} passed${RST}  /  ${RED}${RFAIL} failed${RST}  /  ${RTOTAL} total"
  echo ""
fi

# ── Exit code ─────────────────────────────────────────────────────────────────
(( FAIL == 0 ))
