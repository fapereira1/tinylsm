#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/fuzz/corpus"
ARTIFACTS="$ROOT/fuzz/artifacts"

mkdir -p "$CORPUS" "$ARTIFACTS"

echo "=== Build fuzzer ==="
cmake --preset fuzzer
cmake --build "$ROOT/build/fuzzer" --target fuzz_db

echo "=== Rodando fuzzer (Ctrl+C para parar) ==="
echo "    Corpus:    $CORPUS"
echo "    Artifacts: $ARTIFACTS"
echo ""

"$ROOT/build/fuzzer/fuzz_db" \
  -artifact_prefix="$ARTIFACTS/" \
  -max_len=512 \
  -timeout=10 \
  -print_final_stats=1 \
  "$CORPUS"
