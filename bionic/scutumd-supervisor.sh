#!/data/data/com.termux/files/usr/bin/bash
cd "$(dirname "$0")" || exit 1
: > "$HOME/scutumd.log"
while true; do
  echo "[supervisor] $(date): arrancando scutumd" >> "$HOME/scutumd.log"
  SCUTUM_LIBDIR=/system/lib64 \
  SCUTUM_SOCK="$PREFIX/tmp/host-tmp/scutum.sock" \
  LD_PRELOAD="$HOME/dev/Scutum/libcrashisolate.so" \
    ./scutumd >> "$HOME/scutumd.log" 2>&1
  rc=$?
  echo "[supervisor] scutumd salió con $rc, relanzando en 1s" >> "$HOME/scutumd.log"
  rm -f "$PREFIX/tmp/host-tmp/scutum.sock"
  sleep 1
done
