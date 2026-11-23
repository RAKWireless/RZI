# SPDX-License-Identifier: Apache-2.0
"""Compile and test the patched USP helper with host sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--usp", type=Path, required=True, help="Patched USP checkout")
parser.add_argument("--cc", default="gcc", help="Host C compiler with ASan/UBSan")
args = parser.parse_args()
here = Path(__file__).resolve().parent
helper = args.usp.resolve() / "protocols/lbm_lib/smtc_modem_core/lorawan_packages/fragmented_data_block_transport/v1.0.0"
with tempfile.TemporaryDirectory(prefix="fuota-fec-") as directory:
    binary = Path(directory) / "replay"
    subprocess.run([args.cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
                    "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-DFRAG_MAX_NB=300",
                    "-DFRAG_MAX_SIZE=64", "-I" + str(helper),
                    str(here / "src/main.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(here / "replay.bin")], check=True)
