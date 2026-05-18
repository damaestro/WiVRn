#!/usr/bin/env python3
# Perfetto pftrace summarizer — pairs BEGIN/END by trusted_packet_sequence_id.

import sys, collections, statistics, re

_SUFFIX = re.compile(r' f=\d+ s=\d+$')
def canonical(name):
    return _SUFFIX.sub('', name)

def read_varint(buf, off):
    v = 0; shift = 0
    while True:
        b = buf[off]; off += 1
        v |= (b & 0x7F) << shift
        if not (b & 0x80): return v, off
        shift += 7

def read_tag(buf, off):
    tag, off = read_varint(buf, off)
    return tag >> 3, tag & 7, off

def skip(buf, off, wt):
    if wt == 0: _, off = read_varint(buf, off); return off
    if wt == 1: return off + 8
    if wt == 2:
        ln, off = read_varint(buf, off); return off + ln
    if wt == 5: return off + 4
    raise ValueError(f"unknown wt {wt}")

def parse_track_event(buf, off, end):
    name = None; ev_type = None
    while off < end:
        fid, wt, off = read_tag(buf, off)
        if fid == 23 and wt == 2:
            ln, off = read_varint(buf, off)
            name = buf[off:off+ln].decode("utf-8", "replace"); off += ln
        elif fid == 9 and wt == 0:
            ev_type, off = read_varint(buf, off)
        else:
            off = skip(buf, off, wt)
    return name, ev_type

def summarize(path):
    with open(path, 'rb') as f: buf = f.read()
    counts = collections.Counter()
    durations = collections.defaultdict(list)
    intervals = collections.defaultdict(list)
    last_begin = {}                              # name -> last begin ts
    open_stack = collections.defaultdict(list)   # seq_id -> [(name, ts), ...]
    ts_min = ts_max = None

    off = 0; n = len(buf)
    while off < n:
        fid, wt, off = read_tag(buf, off)
        if fid != 1 or wt != 2:
            off = skip(buf, off, wt); continue
        ln, off = read_varint(buf, off)
        end = off + ln
        ts = None; seq = None; te_name = None; te_type = None
        while off < end:
            f2, wt2, off = read_tag(buf, off)
            if f2 == 8 and wt2 == 0:
                ts, off = read_varint(buf, off)
            elif f2 == 10 and wt2 == 0:
                seq, off = read_varint(buf, off)
            elif f2 == 11 and wt2 == 2:
                ln2, off = read_varint(buf, off)
                te_name, te_type = parse_track_event(buf, off, off+ln2)
                off += ln2
            else:
                off = skip(buf, off, wt2)
        if ts is not None:
            if ts_min is None or ts < ts_min: ts_min = ts
            if ts_max is None or ts > ts_max: ts_max = ts
        if te_type is not None:
            tname = te_name if te_name else "(end)"
            counts[(tname, te_type)] += 1
            if te_type == 1 and te_name and ts is not None:                # BEGIN
                cname = canonical(te_name)
                open_stack[seq].append((cname, ts))
                if cname in last_begin:
                    intervals[cname].append(ts - last_begin[cname])
                last_begin[cname] = ts
            elif te_type == 2 and ts is not None and open_stack[seq]:      # END
                bname, bts = open_stack[seq].pop()
                durations[bname].append(ts - bts)

    span = (ts_max - ts_min) if ts_min else 0
    print(f"\n=== {path} ===")
    print(f"  file size:     {len(buf):,} bytes")
    print(f"  trace span:    {span/1e9:.3f} s")

    # Rows = (name, n, mean_ms, p50_ms, p99_ms, max_ms, rate_hz)
    rows = []
    for name, dur_list in durations.items():
        if not dur_list: continue
        srt = sorted(dur_list)
        p99 = srt[int(len(srt)*0.99)] if len(srt) > 1 else srt[0]
        rate = (len(dur_list) / (span/1e9)) if span else 0
        rows.append((name, len(dur_list),
                     statistics.mean(dur_list)/1e6,
                     statistics.median(dur_list)/1e6,
                     p99/1e6, max(dur_list)/1e6, rate))
    rows.sort(key=lambda r: -r[1])
    print(f"  {'slice':<32} {'n':>6} {'mean ms':>9} {'p50 ms':>8} {'p99 ms':>8} {'max ms':>8} {'rate/s':>8}")
    for r in rows[:30]:
        print(f"  {r[0]:<32} {r[1]:>6} {r[2]:>9.3f} {r[3]:>8.3f} {r[4]:>8.3f} {r[5]:>8.3f} {r[6]:>8.1f}")

if __name__ == '__main__':
    for p in sys.argv[1:]: summarize(p)
