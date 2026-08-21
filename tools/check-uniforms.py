#!/usr/bin/env python3
"""Verify GPUUniforms (dsvp.h) and the HLSL cbuffer (player.c) agree.
A mismatch silently corrupts every uniform after the divergence point."""
import re, sys
REPO = sys.argv[1] if len(sys.argv) > 1 else '.'
COMP = {'float':1,'float2':2,'float3':3,'float4':4,'float4x4':16}
hdr = open(f'{REPO}/src/dsvp.h', encoding='utf-8').read()
src = open(f'{REPO}/src/player.c', encoding='utf-8').read()

cf = []
body = re.search(r'typedef struct GPUUniforms \{(.*?)\} GPUUniforms;', hdr, re.S).group(1)
for line in body.split('\n'):
    line = re.sub(r'/\*.*?\*/', '', line, flags=re.S).strip()
    m = re.match(r'(?:float|int)\s+(\w+)(?:\[(\d+)\])?(?:\[(\d+)\])?\s*;', line)
    if m:
        n = int(m.group(2) or 1) * int(m.group(3) or 1)
        cf.append((m.group(1), n))

sf = []
seg = src[src.index('cbuffer Params'):src.index('dovi_mmr_cp') + 200]
for raw in seg.split('\\n'):
    line = re.sub(r'^[\s"]+', '', raw).strip()          # <- strip ALL leading quotes/space
    m = re.match(r'(?:row_major\s+)?(float4x4|float4|float3|float2|float)\s+(\w+)(?:\[(\d+)\])?\s*;', line)
    if m:
        sf.append((m.group(2), COMP[m.group(1)] * int(m.group(3) or 1)))

if not sf:
    print("PARSER FAILED: no HLSL fields matched — fix the checker, do not trust a verdict")
    sys.exit(2)

bad = [(i, cf[i] if i < len(cf) else None, sf[i] if i < len(sf) else None)
       for i in range(max(len(cf), len(sf)))
       if (cf[i] if i < len(cf) else None) != (sf[i] if i < len(sf) else None)]
tc, ts = sum(f[1] for f in cf), sum(f[1] for f in sf)
print(f"C struct : {len(cf):2d} fields, {tc*4} bytes")
print(f"HLSL cbuf: {len(sf):2d} fields, {ts*4} bytes")
for i, c, s in bad:
    print(f"  MISMATCH idx {i}: C={c} HLSL={s}")
print("layout:", "IDENTICAL — safe" if not bad else "*** MISMATCH ***")
sys.exit(1 if bad else 0)
