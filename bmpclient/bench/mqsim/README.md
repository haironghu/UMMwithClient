# MQSim oracle adapter

Upstream: https://github.com/CMU-SAFARI/MQSim
Pinned revision: `51f0f2d3fed92d88ef4a0fa61a38024b07bf9d16`.
MQSim is copyright its upstream authors, licensed under MIT; the installer retains
its original LICENSE in the downloaded checkout. This directory stores only the
patch; no upstream source tree or binary is vendored.

`oracle.patch` adds opt-in `DSA_ORACLE_QD`, `DSA_ORACLE_BATCHES`,
`DSA_ORACLE_MAPPING`, and `DSA_ORACLE_NAND` environment variables. Unset them to
retain the normal trace replay. In DSA mode column 1 is a strictly increasing
batch ID, batch 0 contains snapshot writes, later batches contain reads. One
NVMe flow, one scenario, 100% trace, one replay, ideal page mapping required.
PPA export reads existing FTL mappings after all host write completions. NAND
transactions are logged before completion handlers can release their requests.

The upstream Makefile does not track header dependencies. ALWAYS clean-build
this patch. Use `scripts/setup_mqsim_oracle.py`, not an incremental `make` after
changing `IO_Flow_Trace_Based.h`.

See `bmpclient/docs/15_MQSim_PPA候选替换实验.md` for scope, commands and metrics.
