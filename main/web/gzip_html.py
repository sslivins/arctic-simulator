#!/usr/bin/env python3
"""
Compress a file using gzip for embedding in firmware.
Usage: gzip_html.py <input_file> <output_file>
"""
import sys
import gzip

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <input_file> <output_file>")
    sys.exit(1)

input_file = sys.argv[1]
output_file = sys.argv[2]

try:
    with open(input_file, 'rb') as f_in:
        data = f_in.read()
    
    compressed = gzip.compress(data, compresslevel=9)
    
    with open(output_file, 'wb') as f_out:
        f_out.write(compressed)
    
    ratio = (1 - len(compressed) / len(data)) * 100
    print(f"Compressed {len(data)} -> {len(compressed)} bytes ({ratio:.1f}% reduction)")
    sys.exit(0)
except Exception as e:
    print(f"Error: {e}")
    sys.exit(1)
