#!/bin/bash
# Download one PoroTomo vertical-DAS record (native + GDR-standardized HDF5)
# from the public OEDI bucket. Usage: download_porotomo.sh [dest_dir]
set -e
D=${1:-$HOME/cae_test_data/porotomo}
B=https://nrel-pds-porotomo.s3.amazonaws.com
F=PoroTomo_iDAS025_160318160317.h5
mkdir -p "$D"
[ -s "$D/dasv_native_$F" ] || curl -sS -o "$D/dasv_native_$F" "$B/DAS/H5/DASV/20160317/$F"
[ -s "$D/dasv_std_$F" ] || curl -sS -o "$D/dasv_std_$F" "$B/DAS/H5-standardized/DASV/20160317/$F"
ls -l "$D"
