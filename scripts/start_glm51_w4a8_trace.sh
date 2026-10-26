#!/usr/bin/env bash
# Reconstructed from the user's successful TP16 / eager launch in this session.
# Ascend-only; this launcher DOES NOT install the custom sfa_v1.py dump hook.
set -euo pipefail
mode="${1:-run}"
case "$mode" in run|--dry-run|--check-only) ;; *) echo "Usage: bash $0 [--dry-run|--check-only]" >&2; exit 2 ;; esac
export MODEL_DIR="${MODEL_DIR:-${HOME}/models/GLM-5.1-w4a8}"
MODEL_DIR="$(python3 -c 'from pathlib import Path; import os; print(Path(os.environ["MODEL_DIR"]).expanduser().resolve())')"
export DSA_DUMP_DIR="${DSA_DUMP_DIR:-${HOME}/dsa_dump_dir}"
export VLLM_USE_MODELSCOPE=true
TP_SIZE="${TP_SIZE:-16}"
API_KEY="${API_KEY:-sk-1234}"
command=(vllm serve "$MODEL_DIR"
  --host 0.0.0.0 --port "${PORT:-8088}"
  --data-parallel-size 1 --tensor-parallel-size "$TP_SIZE"
  --enable-expert-parallel --seed 1024 --served-model-name glm-5
  --max-num-seqs "${MAX_NUM_SEQS:-16}" --max-model-len "${MAX_MODEL_LEN:-192000}"
  --max-num-batched-tokens "${MAX_NUM_BATCHED_TOKENS:-16384}"
  --trust-remote-code --gpu-memory-utilization "${GPU_MEMORY_UTILIZATION:-0.92}"
  --quantization ascend --enable-chunked-prefill --disable-custom-all-reduce --enforce-eager
  --additional-config '{"fuse_muls_add":true,"multistream_overlap_shared_expert":true}'
  --enable-auto-tool-choice --tool-call-parser glm47 --api-key "$API_KEY")
if [[ "$mode" == --dry-run ]]; then
  # Show the exact reconstructed command without printing the configured API key.
  redact=0
  for arg in "${command[@]}"; do
    if (( redact )); then printf '%q ' '<API_KEY>'; redact=0
    else printf '%q ' "$arg"; [[ "$arg" != --api-key ]] || redact=1; fi
  done
  printf '\n'
  exit 0
fi
python3 - <<'PY'
from pathlib import Path
from importlib import metadata, util
import json
import os
model=Path(os.environ['MODEL_DIR']).expanduser().resolve()
if not (model/'config.json').is_file():
    raise SystemExit(f'Missing local config.json: {model}; set MODEL_DIR to the downloaded model directory')
cfg=json.loads((model/'config.json').read_text())
if cfg.get('model_type')!='glm_moe_dsa' or cfg.get('index_topk')!=2048:
    raise SystemExit('Unexpected model config; expected glm_moe_dsa and native index_topk=2048')
if not (model/'quant_model_description.json').is_file():
    raise SystemExit('Missing Ascend quant_model_description.json')
shards=set()
for p in model.glob('*.index.json'):
    data=json.loads(p.read_text())
    shards.update(data.get('weight_map',{}).values())
if not shards or any(not (model/s).is_file() or (model/s).stat().st_size==0 for s in shards):
    raise SystemExit('Missing weight shard index or missing/empty shards')
print(f'Local model OK: {model}, layers={cfg.get("num_hidden_layers")}, index_topk=2048, shards={len(shards)}')
for name in ('vllm','vllm-ascend','torch','torch-npu','transformers'):
    try: print(f'{name}={metadata.version(name)}')
    except metadata.PackageNotFoundError:print(f'{name}=NOT INSTALLED')
spec=util.find_spec('vllm_ascend')
if spec and spec.origin:
    path=Path(spec.origin).parent/'attention/sfa_v1.py'
    if path.is_file():
        text=path.read_text(errors='replace')
        print(f'Active sfa_v1.py: {path}')
        for marker in ('_trace_indexer_candidates','candidate_scores','has_reference_candidates','DSA_DUMP_DIR'):
            print(f'  source marker {marker}: {marker in text}')
        print('Marker presence is only a diagnostic; validate an actual decode .pt before full collection.')
else:
    print('vllm_ascend is unavailable: this launcher is for Ascend, not NVIDIA GPUs.')
PY
if [[ "$mode" == --check-only ]]; then exit 0; fi
command -v vllm >/dev/null || { echo 'vllm command missing in this environment' >&2; exit 1; }
python3 -c 'import torch_npu; import vllm_ascend' || { echo 'Ascend runtime unavailable; restore the matching inference image' >&2; exit 1; }
mkdir -p "$DSA_DUMP_DIR"
echo "Start GLM-5.1 W4A8: TP=$TP_SIZE, dump=$DSA_DUMP_DIR, date=$(date '+%Y-%m-%d %H:%M:%S')"
exec "${command[@]}"
