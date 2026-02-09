#!/usr/bin/env bash
set -e

for i in 1; do
  pandoc src/assignment$i.md \
    --standalone \
    --syntax-highlighting pygments \
    --mathjax \
    --css ../static/style.css \
    --css ../static/override.css \
    -o public/assignment$i.html
done

pandoc src/index.md \
  --standalone \
  --syntax-highlighting pygments \
  --mathjax \
  --css ../static/style.css \
  --css ../static/override.css \
  -o public/index.html
