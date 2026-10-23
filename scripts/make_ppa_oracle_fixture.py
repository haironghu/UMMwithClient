#!/usr/bin/env python3
"""Make deliberately channel-concentrated JSON fixtures, NOT real GLM traces."""
import argparse
import json
from pathlib import Path


def make_fixture(directory, layers=(2,10), steps=2, context=10000):
    if steps < 1 or context < 9000:
        raise ValueError('Fixture requires steps>=1 and context>=9000')
    directory.mkdir(parents=True,exist_ok=False)
    for layer in layers:
        for step in range(steps):
            seq=context+step
            # Reference 4-channel CWDP: four 4KiB slots per 16KiB NAND page.
            # Intentional stress case, used only to validate the mechanism.
            native=[t for t in range(seq-1) if (t//4)%4==0][:2047]+[seq-1]
            extra=[t for t in range(seq-1) if (t//4)%4!=0 and t not in native][:352]
            ids=native+extra
            row=dict(layer=f'model.layers.{layer}.self_attn.attn',indices=native,
                     seq_lens_key=[seq],seq_lens_query=[1],has_reference_candidates=True,
                     candidate_indices=ids,candidate_scores=list(range(2400,0,-1)))
            (directory/f'model_layers_{layer}_s{seq}.json').write_text(json.dumps(row)+'\n')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out-dir',type=Path,required=True)
    p.add_argument('--steps',type=int,default=2)
    a=p.parse_args();make_fixture(a.out_dir,steps=a.steps)
