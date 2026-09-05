#!/usr/bin/env python3
"""
provenance_scan.py - is any of this repository's code copied from somewhere?

Much of this project was written with AI assistance, which makes "we wrote it
ourselves" a claim that deserves a check rather than an assurance. This is that
check: it tokenises our sources and the upstream projects we could plausibly
have been derived from, throws away comments and string literals, and looks for
runs of identical code.

    python3 tools/provenance_scan.py <upstream-dir> [<upstream-dir> ...] <our-src-dir>

for example, after cloning the candidates side by side:

    python3 tools/provenance_scan.py /tmp/OptiScaler_DLSSNR /tmp/DLSS5-Feeder \
                                     /tmp/renodx src

WHAT THE OUTPUT MEANS. Copied code produces LONG RUNS of consecutive matching
windows. Isolated matches do not: filling a D3D12_RESOURCE_BARRIER, or writing
out an NGX function signature, is identical in every project that does it
because the field names and their order are fixed by somebody else's header.
Read the "runs>=10" column first; a percentage on its own means very little.

TWO DELIBERATE CHOICES, both of which suppress false positives that made the
first version of this useless:

  - String literals are replaced by a placeholder, so that a match is about
    code structure rather than about two projects logging similar words.
  - A window must contain at least 6 DISTINCT identifiers to be counted.
    Without that rule, runs of consecutive string arguments - ImGui text calls,
    say - collapse to identical placeholder sequences and match everything,
    which produced a spurious 11% "similarity" on the first run.

WHAT IT CANNOT DO. It checks the repositories you point it at and no others, it
cannot detect heavily reworded copying, and it is only as current as the last
time it was run. It is evidence, not proof.
"""
import re, os, sys, hashlib
from collections import defaultdict

def norm_tokens(path):
    try:
        s = open(path, encoding='utf-8', errors='ignore').read()
    except Exception:
        return []
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)      # block comments
    s = re.sub(r'//[^\n]*', ' ', s)                    # line comments
    s = re.sub(r'"(?:[^"\\]|\\.)*"', ' "S" ', s)       # string literals -> placeholder
    toks = re.findall(r'[A-Za-z_][A-Za-z_0-9]*|[0-9]+|[^\sA-Za-z_0-9]', s)
    return toks

N = 14   # shingle length in tokens

def shingles(toks):
    for i in range(len(toks) - N + 1):
        w = toks[i:i+N]
        # Skip windows dominated by placeholders/punctuation: they match
        # everything and mean nothing. A window must carry at least 6 distinct
        # identifiers to count as evidence of anything.
        ids = set(t for t in w if t.isidentifier() and t != 'S')
        if len(ids) < 6:
            continue
        h = hashlib.blake2b(' '.join(w).encode(), digest_size=8).hexdigest()
        yield i, h

# index upstream
index = defaultdict(list)
roots = sys.argv[1:-1]
mine  = sys.argv[-1]
files = 0
for root in roots:
    for dp, _, fn in os.walk(root):
        if '.git' in dp: continue
        for f in fn:
            if not f.endswith(('.c','.cc','.cpp','.cxx','.h','.hh','.hpp','.hxx')): continue
            p = os.path.join(dp, f)
            t = norm_tokens(p)
            if len(t) < N: continue
            files += 1
            for i, h in shingles(t):
                index[h].append((p, i))
print(f"indexed {files} upstream files, {len(index)} distinct {N}-token shingles\n")

for f in sorted(os.listdir(mine)):
    if not f.endswith(('.cpp','.hpp','.h')): continue
    p = os.path.join(mine, f)
    t = norm_tokens(p)
    hits = []
    for i, h in shingles(t):
        if h in index:
            hits.append((i, index[h][0]))
    total = max(1, len(t) - N + 1)
    # collapse consecutive positions into runs
    runs = []
    for i, src in hits:
        if runs and i == runs[-1][1] + 1 and runs[-1][2][0] == src[0]:
            runs[-1][1] = i
        else:
            runs.append([i, i, src])
    long_runs = [r for r in runs if (r[1] - r[0]) >= 10]
    print(f"{f:24s} tokens={len(t):7d}  matching shingles={len(hits):5d} ({100.0*len(hits)/total:5.2f}%)  runs>=10: {len(long_runs)}")
    for r in sorted(long_runs, key=lambda r: r[0]-r[1])[:5]:
        span = ' '.join(t[r[0]:r[0]+N+ (r[1]-r[0])])
        print(f"    run of {r[1]-r[0]+N} tokens  <- {r[2][0]}")
        print(f"      {span[:150]}")

