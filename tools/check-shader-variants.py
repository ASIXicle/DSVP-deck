#!/usr/bin/env python3
"""Preprocess every compile-time variant of the shared YUV fragment
shader and fail loudly if any variant is structurally broken.

Born 2026-08-20: the DSVP_DIRECT early-return left the kernel
branches' shared tail referencing variables the direct build never
declares — HLSL compile failed on-device, the WARN fell back to the
fixed pipeline, and a whole field run silently measured the wrong
thing. Both deck pipeline variants compile from ONE source string
(the shared-source trap), so every define combination must be
checked, not just the one being edited.

Checks per variant: C preprocessor succeeds, braces balance, and
each sampler function either kept its kernel + tail or collapsed to
its early return — never half of each. Run alongside
tools/check-uniforms.py after ANY edit to hlsl_yuv_planar_frag or
to the variant define matrix.
"""
import re
import subprocess
import sys

SRC = "src/player.c"

VARIANTS = {
    "fixed":   {"DSVP_DILATE": 0, "DSVP_DIRECT": 0, "DSVP_SCALE2X": 0},
    "dilated": {"DSVP_DILATE": 1, "DSVP_DIRECT": 0, "DSVP_SCALE2X": 0},
    "direct":  {"DSVP_DILATE": 0, "DSVP_DIRECT": 1, "DSVP_SCALE2X": 0},
    "scale2x": {"DSVP_DILATE": 0, "DSVP_DIRECT": 0, "DSVP_SCALE2X": 1},
}
# PQ_LUT axis: both values must preprocess for every variant above.
PQ_LUT_VALUES = (0, 1)
# Chroma anti-ring axis (2026-08-20): the container-keyed predicate is
# an #ifdef/#else pair in the shared source — both sides must
# preprocess for every variant. 0 = default (container-keyed),
# 1 = DSVP_CHROMA_AR=hdr fallback.
CHROMA_AR_VALUES = (0, 1)

FUNCS = ("float sample_lanczos", "float sample_catmull(",
         "float2 sample_catmull_rg")


def extract_shader(path):
    src = open(path).read()
    start = src.index("static const char hlsl_yuv_planar_frag[] =")
    end = src.index('";\n', start)
    pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', src[start:end + 2])
    return "".join(p.encode().decode("unicode_escape") for p in pieces)


def fn_body(out, decl):
    m = re.search(re.escape(decl) + r".*?\n\}", out, re.S)
    if not m:
        return None
    return m.group(0)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    shader = extract_shader(f"{root}/{SRC}")
    failures = 0
    for name, defs in VARIANTS.items():
      for lut in PQ_LUT_VALUES:
        for car in CHROMA_AR_VALUES:
            # Per-variant baseline: "ok" was printed only while the
            # CUMULATIVE count was zero, so after the first failure
            # every later passing variant went unreported — the
            # coverage evidence vanished exactly when it was needed
            # (Knot audit finding 12).
            tag_baseline = failures
            d = dict(defs, DSVP_PQ_LUT=lut)
            if car:
                d["DSVP_CHROMA_AR_HDR"] = 1
            prefix = "".join(f"#define {k} {v}\n" for k, v in d.items())
            r = subprocess.run(["cpp", "-P", "-"], input=prefix + shader,
                               capture_output=True, text=True)
            tag = f"{name}/lut{lut}" + ("/ar-hdr" if car else "")
            if r.returncode != 0:
                print(f"FAIL {tag}: cpp error\n{r.stderr[:400]}")
                failures += 1
                continue
            out = r.stdout
            if out.count("{") != out.count("}"):
                print(f"FAIL {tag}: unbalanced braces "
                      f"{out.count('{')} vs {out.count('}')}")
                failures += 1
                continue
            for decl in FUNCS:
                body = fn_body(out, decl)
                if body is None:
                    print(f"FAIL {tag}: {decl} not found")
                    failures += 1
                    continue
                kernel = "wsum" in body
                early = "return tex.SampleLevel(samp, uv, 0).r;" in body
                is_direct_luma = (name == "direct"
                                  and decl.startswith("float sample_lanczos"))
                if is_direct_luma:
                    if kernel or not early:
                        print(f"FAIL {tag}: {decl} — direct build must be "
                              f"a bare fetch (kernel={kernel}, "
                              f"fetch={early})")
                        failures += 1
                else:
                    if not kernel:
                        print(f"FAIL {tag}: {decl} lost its kernel/tail")
                        failures += 1
                    if decl.startswith("float sample_lanczos"):
                        want_const = (name == "scale2x")
                        has_const = "W25" in body
                        has_sin = "lanczos2(float(j)" in body
                        if want_const and (not has_const or has_sin):
                            print(f"FAIL {tag}: scale2x lanczos must use "
                                  f"constant weights (W25={has_const}, "
                                  f"sin-path={has_sin})")
                            failures += 1
                        if not want_const and has_const:
                            print(f"FAIL {tag}: W25 leaked into {name}")
                            failures += 1
            if failures == tag_baseline:
                print(f"ok   {tag}")
    if failures:
        print(f"{failures} FAILURE(S)")
        return 1
    print("all shader variants structurally sound")
    return 0


if __name__ == "__main__":
    sys.exit(main())
