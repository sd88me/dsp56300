import struct, mmap, sys

REC = struct.Struct('<B7xIQQIQQ')
SZ = REC.size
assert SZ == 48

def open_mm(path):
    f = open(path, 'rb')
    mm = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
    return f, mm

def rec_at(mm, i):
    return REC.unpack_from(mm, i*SZ)

def n_recs(mm):
    return len(mm)//SZ

def uc_gen(mm):
    n = n_recs(mm)
    for i in range(n):
        kind, pc, a, b, sr, cycles, instr = rec_at(mm, i)
        if kind == 1:
            yield i, pc, cycles

def dsp_gen(mm):
    n = n_recs(mm)
    for i in range(n):
        kind, pc, a, b, sr, cycles, instr = rec_at(mm, i)
        if kind == 0:
            yield i, pc, a, b, sr, cycles, instr

f1, m1 = open_mm('/home/sam/scratch-gm/trace_int2.bin')
f2, m2 = open_mm('/home/sam/scratch-gm/trace_rc3.bin')
print('int total records', n_recs(m1), 'rc total records', n_recs(m2))

# --- uC stream diff: true call-order comparison, O(1) memory ---
g1 = uc_gen(m1)
g2 = uc_gen(m2)
count = 0
mismatch = None
while True:
    try:
        e1 = next(g1)
    except StopIteration:
        e1 = None
    try:
        e2 = next(g2)
    except StopIteration:
        e2 = None
    if e1 is None or e2 is None:
        print(f'reached end of one uC stream after {count} matched events (int exhausted={e1 is None}, rc exhausted={e2 is None})')
        break
    if (e1[1], e1[2]) != (e2[1], e2[2]):
        mismatch = (count, e1, e2)
        break
    count += 1
    if count % 2000000 == 0:
        print(f'  ...{count} uC events matched so far', flush=True)

if mismatch:
    j, e1, e2 = mismatch
    print(f'FIRST uC divergence at uc-event #{j}: int(idx={e1[0]},pc={e1[1]:06x},cycles={e1[2]}) rc(idx={e2[0]},pc={e2[1]:06x},cycles={e2[2]})')
else:
    print(f'uC streams matched fully over the overlap ({count} events) -- no ordering/pc/cycles divergence')

# --- dsp stream diff by matching dspInstr counter, two-pointer merge (both monotonic) ---
g1d = dsp_gen(m1)
g2d = dsp_gen(m2)
a = next(g1d, None)
b = next(g2d, None)
checked = 0
dmismatch = None
while a is not None and b is not None:
    ia, ib = a[6], b[6]
    if ia == ib:
        checked += 1
        if a[1:6] != b[1:6]:
            dmismatch = (ia, a, b)
            break
        a = next(g1d, None)
        b = next(g2d, None)
    elif ia < ib:
        a = next(g1d, None)
    else:
        b = next(g2d, None)
    if checked and checked % 2000000 == 0:
        print(f'  ...{checked} dsp instr-counter matches checked so far', flush=True)

print('checked', checked, 'matching dspInstr counters')
if dmismatch:
    instr, a, b = dmismatch
    print('DSP MISMATCH at dspInstr', instr)
    print(' int:', a)
    print(' rc :', b)
else:
    print('no dsp mismatch found at any matching dspInstr counter in the overlap')
