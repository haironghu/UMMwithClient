"""Convert external token traces without loading a model."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from bmpclient.bench.convert_trace import main

if __name__ == '__main__':
    main()
