"""Run the actual SparseKVStore host-path benchmark."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from bmpclient.bench.layout_replay import main

if __name__ == '__main__':
    main()
