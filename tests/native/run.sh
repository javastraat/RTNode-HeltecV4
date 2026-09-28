#!/bin/sh
# Builds and runs the host-side unit tests in this directory. These cover
# logic that compiles without Arduino (parsers, codecs, state machines), so
# they need no board. Firmware behaviour is covered by the pytest suite.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

status=0
for test_source in "$here"/test_*.cpp; do
    name=$(basename "$test_source" .cpp)
    c++ -std=c++17 -Wall -Wextra -Werror -o "$out/$name" "$test_source"
    "$out/$name" || status=1
done
exit $status
