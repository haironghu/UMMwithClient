"""Entry point: python3 bmpclient/scripts/bench_single_ssd.py --help."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from bmpclient.bench.single_ssd import main

if __name__ == "__main__":
    main()
