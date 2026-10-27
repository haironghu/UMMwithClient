#!/usr/bin/env bash
# Run on the NEW Ascend host (or a download host with a shared model directory).
set -euo pipefail
export MODEL_ID="${MODEL_ID:-Eco-Tech/GLM-5.1-w4a8}"
export MODEL_DIR="${MODEL_DIR:-${HOME}/models/GLM-5.1-w4a8}"
python3 - <<'PY'
from pathlib import Path
import hashlib
import json
import os

# Explicit per-process workaround for a TLS-intercepting network. This is our
# launcher option, not a ModelScope SDK environment variable. Default: verify.
if os.environ.get('GLM_DOWNLOAD_INSECURE_SSL', '0') == '1':
    import requests
    original_request = requests.sessions.Session.request

    def insecure_request(self, method, url, **kwargs):
        kwargs['verify'] = False
        return original_request(self, method, url, **kwargs)

    requests.sessions.Session.request = insecure_request
    print('[WARNING] TLS certificate verification is DISABLED for this download '
          'process; server identity and protection against interception are lost.', flush=True)

from modelscope import snapshot_download

repo=os.environ['MODEL_ID']
target=Path(os.environ['MODEL_DIR']).expanduser().resolve()
kwargs=dict(model_id=repo,local_dir=str(target))
revision=os.environ.get('MODEL_REVISION')
if revision: kwargs['revision']=revision
print(f'Download {repo} to {target}; resume by rerunning with the same path.',flush=True)
actual=Path(snapshot_download(**kwargs)).resolve()
config=json.loads((actual/'config.json').read_text())
if config.get('model_type')!='glm_moe_dsa' or config.get('num_hidden_layers')!=78:
    raise RuntimeError('Unexpected GLM-5.1 architecture/layer count; inspect downloaded config')
if config.get('index_topk')!=2048:
    raise RuntimeError('Expected native index_topk=2048; do not change it to 2400 for candidate tracing')
if not (actual/'quant_model_description.json').is_file():
    raise RuntimeError('Ascend quant_model_description.json missing')
indices=[]
for p in actual.glob('*.index.json'):
    d=json.loads(p.read_text())
    if isinstance(d.get('weight_map'),dict):indices.append(d)
if not indices:raise RuntimeError('No weight shard index found')
shards={name for d in indices for name in d['weight_map'].values()}
missing=[name for name in shards if not (actual/name).is_file() or (actual/name).stat().st_size==0]
if missing:raise RuntimeError(f'Missing/empty weight shards: {missing[:10]}')
record=dict(model_id=repo,requested_revision=revision or 'SDK default (mutable)',local_path=str(actual),
            tls_certificate_verification=os.environ.get('GLM_DOWNLOAD_INSECURE_SSL', '0') != '1',
            config_sha256=hashlib.sha256((actual/'config.json').read_bytes()).hexdigest(),
            weight_shards=len(shards),weight_shard_bytes=sum((actual/name).stat().st_size for name in shards),
            note='File existence/size checks only; SDK handles downloads. Record a fixed upstream revision for exact reproduction.')
(actual/'download_provenance.json').write_text(json.dumps(record,indent=2)+'\n')
print(f'Ready: MODEL_DIR={actual}; {len(shards)} nonempty weight shards',flush=True)
PY
