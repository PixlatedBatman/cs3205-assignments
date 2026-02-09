#!/usr/bin/env bash
set -e

INPUT="src/assignment1.md"
OUTPUT_MD="/tmp/assignment1_expanded.md"
OUTPUT_HTML="public/assignment1.html"
CODE_DIR="static/codes/assignment1"

cp "$INPUT" "$OUTPUT_MD"

for file in "$CODE_DIR"/*.py; do
  name=$(basename "$file")

  sed -i "/{{CODE:$name}}/{
    r $file
    d
  }" "$OUTPUT_MD"
done

pandoc "$OUTPUT_MD" \
  --standalone \
  --syntax-highlighting pygments \
  --mathjax \
  --css ../static/style.css \
  --css ../static/override.css \
  -o "$OUTPUT_HTML"
