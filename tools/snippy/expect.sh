#!/bin/bash
# What CPython gives for each Snippy test program (the Snippy plan, part 5.2's gate), written
# beside it: <name>.py.out its output, <name>.py.err its exit code, then the line and function of
# each frame of a traceback, then the traceback's last line. Run in WSL, where CPython 3.14 is:
#   wsl -e bash tools/snippy/expect.sh tests/snippy_programs/run [name.py...]
# Hashing is made fixed (PYTHONHASHSEED=0), as Snippy's is, so that sets of strings come out alike.
# No __pycache__ is left beside the programs (PYTHONDONTWRITEBYTECODE).
dir="$1"
shift
cd "$dir" || exit 1
files="$@"
if [ -z "$files" ]; then files=$(ls *.py *.snippy | grep -v "^_"); fi
for f in $files; do
  rm -f -- *.tmp   # what a program leaves (mod_io.py's files), gone before and after each
  PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 timeout 60 python3.14 "$f" > "$f.out" 2> "$f.stderr" < /dev/null
  code=$?
  {
    echo "$code"
    grep -E '^  File ".*", line [0-9]+, in ' "$f.stderr" | sed -E 's/^  File ".*", line ([0-9]+), in (.*)$/line \1, in \2/'
    tail -n 1 "$f.stderr"
  } > "$f.err"
  rm -f "$f.stderr"
  rm -f -- *.tmp
done
