#!/usr/bin/env bash
# Derive + download the PvZ2 Reflourished APK without transcribing the signed CDN path:
# fetch the Malavida download page, pull the dw.malavida.com href out of it, download that.
set -u
PAGE="https://pvz-2-reflourished.en.malavida.com/android/download"
OUT="${1:-C:/Users/Aiden/Desktop/HuskApp/Games/pvz2-reflourished-9.6.1-rfl.apk}"
UA="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36"

echo "== fetching page =="
curl -sL -A "$UA" -o "C:/Users/Aiden/AppData/Local/Temp/malavida.html" -w 'page http=%{http_code} bytes=%{size_download}\n' "$PAGE"
echo "== extracting the CDN link =="
URL=$(grep -oE 'https://dw\.malavida\.com/[A-Za-z0-9/+=_-]+' "C:/Users/Aiden/AppData/Local/Temp/malavida.html" | head -1)
if [ -z "$URL" ]; then echo "NO LINK FOUND (page may be JS-rendered)"; exit 1; fi
echo "link length: ${#URL}"
echo "== downloading =="
curl -L --retry 3 --retry-delay 2 -A "$UA" -e "$PAGE" \
  -o "$OUT" -w 'apk http=%{http_code} bytes=%{size_download} type=%{content_type}\n' "$URL"
ls -l "$OUT"
file "$OUT" 2>/dev/null
