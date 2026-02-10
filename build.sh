#!/usr/bin/env bash
set -e

SRC_DIR="src"
OUT_DIR="public"
CODE_BASE_DIR="static/codes"

CSS1="../static/style.css"
CSS2="../static/override.css"

mkdir -p "$OUT_DIR"

for md in "$SRC_DIR"/*.md; do
  name=$(basename "$md" .md)

  OUTPUT_MD="/tmp/${name}_expanded.md"
  OUTPUT_HTML="$OUT_DIR/${name}.html"

  cp "$md" "$OUTPUT_MD"

  # If a matching code directory exists, expand code placeholders
  CODE_DIR="$CODE_BASE_DIR/$name"
  if [[ -d "$CODE_DIR" ]]; then
    for file in "$CODE_DIR"/*.py; do
      [[ -e "$file" ]] || continue
      fname=$(basename "$file")

      sed -i "/{{CODE:$fname}}/{
        r $file
        d
      }" "$OUTPUT_MD"
    done
  fi

  pandoc "$OUTPUT_MD" \
    --standalone \
    --syntax-highlighting pygments \
    --mathjax \
    --css "$CSS1" \
    --css "$CSS2" \
    -o "$OUTPUT_HTML"

  echo "Built: $OUTPUT_HTML"
done
