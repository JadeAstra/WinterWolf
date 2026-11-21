#!/bin/bash
# Runs each Snippy test program with ww and compares what it gives with what CPython gave
# (tools/snippy/expect.sh): its output, exit code, traceback lines and last line of errors.
#   bash tools/snippy/check.sh <ww> tests/snippy_programs/run [name.py...]
ww="$1"
dir="$2"
shift 2
cd "$dir" || exit 1
files="$@"
if [ -z "$files" ]; then files=$(ls *.py *.snippy | grep -v "^_"); fi
cr=$(printf '\r')
bad=0
for f in $files; do
  "$ww" "$f" > /tmp/snippy.raw 2> /tmp/snippy.rawerr < /dev/null
  code=$?
  sed "s/$cr\$//" /tmp/snippy.raw > /tmp/snippy.out
  sed "s/$cr\$//" /tmp/snippy.rawerr > /tmp/snippy.stderr
  {
    echo "$code"
    grep -E '^  File ".*", line [0-9]+, in ' /tmp/snippy.stderr | sed -E 's/^  File ".*", line ([0-9]+), in (.*)$/line \1, in \2/'
    tail -n 1 /tmp/snippy.stderr
  } > /tmp/snippy.err
  if ! cmp -s /tmp/snippy.out "$f.out" || ! cmp -s /tmp/snippy.err "$f.err"; then
    bad=$((bad + 1))
    echo "=== $f differs"
    diff -a "$f.out" /tmp/snippy.out | head -${LINES_SHOWN:-12}
    diff -a "$f.err" /tmp/snippy.err | head -8
    if [ -s /tmp/snippy.stderr ] && ! cmp -s /tmp/snippy.err "$f.err"; then head -5 /tmp/snippy.stderr; fi
  fi
done
echo "$bad differ"
