#!/bin/sh
# Downloads the Wolfenstein: Enemy Territory 2.60b game data (freeware, not
# GPL -- see the EULA shipped with the original game) into data/etmain/.
# Uses the same mirror as ET: Legacy's installer.
set -e
cd "$(dirname "$0")/.."
mkdir -p data/etmain
for p in pak0 pak1 pak2 mp_bin; do
  if [ ! -f "data/etmain/$p.pk3" ]; then
    echo "fetching $p.pk3"
    curl -fL -o "data/etmain/$p.pk3.part" "https://mirror.etlegacy.com/etmain/$p.pk3"
    mv "data/etmain/$p.pk3.part" "data/etmain/$p.pk3"
  fi
done
shasum -a 1 -c <<SUMS
381660cd64aff71b87919c0883fb6b9d082a3835  data/etmain/pak0.pk3
25ab5c29c9534f7593e0dae33052478bb5b8a64e  data/etmain/pak1.pk3
64301bab7170e8e675e9ea8b94b4f77e1091e6c9  data/etmain/pak2.pk3
6a9f0a4c6b6a94329577fbcf08207ac23364b71c  data/etmain/mp_bin.pk3
SUMS
