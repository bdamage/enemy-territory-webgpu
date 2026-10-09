#!/bin/sh
# Downloads the game data into data/ (never committed):
#   data/etmain/pak0-2.pk3, mp_bin.pk3   Wolfenstein: Enemy Territory 2.60b
#                                        (freeware, not GPL -- see the
#                                        original EULA), from the mirror
#                                        ET: Legacy's installer uses
#   data/legacy/legacy_v2.86.1.pk3       the official ET: Legacy 2.86.1 mod
#                                        pak, from etlegacy.com
set -e
cd "$(dirname "$0")/.."
mkdir -p data/etmain data/legacy
for p in pak0 pak1 pak2 mp_bin; do
  if [ ! -f "data/etmain/$p.pk3" ]; then
    echo "fetching $p.pk3"
    curl -fL -o "data/etmain/$p.pk3.part" "https://mirror.etlegacy.com/etmain/$p.pk3"
    mv "data/etmain/$p.pk3.part" "data/etmain/$p.pk3"
  fi
done
if [ ! -f data/legacy/legacy_v2.86.1.pk3 ]; then
  echo "fetching etlegacy-mod-v2.86.1.zip"
  tmp=$(mktemp -d)
  curl -fL -o "$tmp/mod.zip" "https://www.etlegacy.com/download/file/783"
  unzip -q -o "$tmp/mod.zip" legacy_v2.86.1.pk3 -d data/legacy
  rm -rf "$tmp"
fi
shasum -a 1 -c <<SUMS
381660cd64aff71b87919c0883fb6b9d082a3835  data/etmain/pak0.pk3
25ab5c29c9534f7593e0dae33052478bb5b8a64e  data/etmain/pak1.pk3
64301bab7170e8e675e9ea8b94b4f77e1091e6c9  data/etmain/pak2.pk3
6a9f0a4c6b6a94329577fbcf08207ac23364b71c  data/etmain/mp_bin.pk3
cf5c02bd76552e9e0ab11a660677214c0af915c0  data/legacy/legacy_v2.86.1.pk3
SUMS
