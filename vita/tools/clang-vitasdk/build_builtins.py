# Builds compiler-rt's builtins for the Vita (libclang_rt.builtins-arm.a): the generic C, then ARM's assembly in place of any
# generic file of the same name, as compiler-rt's own CMake filters them.
import os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
B = sys.argv[1]; out = sys.argv[2]; cc = sys.argv[3]
cm = open(os.path.join(B, 'CMakeLists.txt')).read()
def lst(name):
    m = re.search(r'set\(' + name + r'\s+(.*?)\)', cm, re.S)
    items = []
    for t in m.group(1).split():
        if t.startswith('${'):
            items += lst(t[2:-1])
        elif not t.startswith('#'):
            items.append(t)
    return items
generic = lst('GENERIC_SOURCES')
arm = lst('arm_or_thumb2_base_SOURCES') + lst('arm_sync_SOURCES') + lst('arm_EABI_SOURCES')
arm = [a for a in arm if a.startswith('arm/')]
armBase = {os.path.splitext(os.path.basename(a))[0] for a in arm}
srcs = [g for g in generic if os.path.splitext(os.path.basename(g))[0] not in armBase] + arm
srcs = sorted(set(srcs))
os.makedirs('obj', exist_ok=True)
def build(s):
    o = os.path.join('obj', s.replace('/', '_') + '.o')
    r = subprocess.run([cc, '-c', '-O2', '-fno-builtin', '-std=c11', '-DVISIBILITY_HIDDEN', '-ffreestanding', os.path.join(B, s), '-o', o], capture_output=True, text=True)
    return (s, o, r.returncode, r.stderr)
objs = []
with ThreadPoolExecutor(2) as ex:
    for s, o, rc, err in ex.map(build, srcs):
        if rc: print('FAIL', s, err[:400])
        else: objs.append(o)
subprocess.check_call(['llvm-ar', 'rcs', out] + objs)
print(len(objs), 'of', len(srcs), 'built')
