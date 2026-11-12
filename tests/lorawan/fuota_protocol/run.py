#!/usr/bin/env python3
"""Compile real USP v1 parsers/FEC with host doubles and ASan/UBSan (GCC required)."""
from pathlib import Path
import argparse
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', type=Path)
args = parser.parse_args()
test = Path(__file__).resolve().parent
workspace = test.parents[3]
usp = workspace/'modules/lib/usp'
build = args.build_dir or workspace/'build/fuota-protocol'
build.mkdir(parents=True, exist_ok=True)
includes = sorted({str(p.parent) for p in usp.rglob('*.h')})
pkg = usp/'protocols/lbm_lib/smtc_modem_core/lorawan_packages'
sources = [test/'main.c', test/'fec_fixture.c',
           pkg/'fragmented_data_block_transport/v1.0.0/lorawan_fragmentation_package_v1.0.0.c',
           pkg/'remote_multicast_setup/v1.0.0/lorawan_remote_multicast_setup_package_v1.0.0.c',
           pkg/'application_layer_clock_synchronization/v1.0.0/lorawan_alcsync_v1.0.0.c']
flags = ['-O1','-g','-ffunction-sections','-fdata-sections','-fno-omit-frame-pointer',
         '-fsanitize=address,undefined','-fno-sanitize-recover=all','-DSX126X','-DNUMBER_OF_STACKS=1',
         '-DADD_FUOTA=1','-DRP2_103','-DADD_CLASS_C','-DADD_CLASS_B','-DSMTC_MULTICAST',
         '-DFRAG_MAX_NB=200','-DFRAG_MAX_SIZE=64']
binary = build/'fuota_protocol_tests'
subprocess.run(['gcc',*flags,*['-I'+p for p in includes],*map(str,sources),
                '-Wl,--gc-sections','-o',str(binary)], check=True)
subprocess.run([str(binary)], check=True)
