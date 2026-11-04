import sys, os, random, subprocess, json
sys.path.insert(0, os.path.dirname(__file__)); sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import ref_plover as r
from steno_keys import parse_steno, format_stroke
HERE = os.path.dirname(os.path.abspath(__file__))
ASSET = os.path.join(HERE, '..', 'assets')
BIN = sys.argv[1]; DICT = os.path.join(HERE, '..', '..', 'dict', 'dict.bin')
import convert_dict as cd
d = r.make([ASSET+'/main.json', ASSET+'/commands.json', HERE+'/../user.json'])
# Plover stroke from text
def pstroke(m):
    return r.Stroke.__call__ if False else r_stroke(m)
from plover.steno import Stroke
def r_stroke(m):
    return Stroke(format_stroke(m)) if False else Stroke.from_steno(format_stroke(m)) if hasattr(Stroke,'from_steno') else None
def ref(strokes):
    from plover.steno import normalize_steno
    class S(str): pass
    return r.run(d, [Stroke_from(format_stroke(m)) for m in strokes])
import plover.steno as ps
def Stroke_from(s):
    return ps.Stroke.from_steno(s) if hasattr(ps.Stroke,'from_steno') else ps.Stroke(s)
def mine(strokes):
    inp = ''.join('%x\n' % m for m in strokes)
    p = subprocess.run([BIN, DICT], input=inp, capture_output=True, text=True)
    if p.returncode: return 'CRASH:' + p.stderr[:300]
    return p.stdout
keys = [tuple(k.split('/')) for k in json.load(open(ASSET+'/main.json'))]
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
MJ = json.load(open(ASSET+'/main.json')); MJ.update(json.load(open(ASSET+'/commands.json'))); MJ.update(json.load(open(HERE+'/../user.json')))
ok = lambda k: d.get(tuple(k)) is not None
meta = [tuple(k.split('/')) for k,v in MJ.items() if '{' in v and 'PLOVER' not in v and ok(k.split('/'))]
entries = [k for k in keys if len(k) <= 3 and ok(k) and 'PLOVER' not in MJ['/'.join(k)]]
EXTRA = ['KAT','TKOG','-G','-D','-S','-Z','1','12','#TPH','STKPW','A*','*P','-FRPBLGTSDZ','KO*P','K*UT','PA*PS','S*EL','*UPB','R*FPB','TK*FS','TK*PLT']
special = ['*','R*PT','TK*FS','TKPWHRAOUPB']
bad = 0; N = int(sys.argv[3]) if len(sys.argv) > 3 else 300
for i in range(N):
    seq = []
    for _ in range(random.randint(2, 8)):
        c = random.random()
        k = random.choice(meta if random.random()<.4 else entries if random.random()<.7 else [(x,) for x in EXTRA])
        seq += [parse_steno(x)[0] for x in k]
        if c < .12: seq.append(parse_steno('*')[0])
        elif c < .17: seq.append(parse_steno('R*PT')[0])
    a, b = ref(seq), mine(seq)
    if a != b:
        bad += 1
        if bad <= 8: print('DIFF', [format_stroke(m) for m in seq], '\n  plover:', repr(a), '\n  mine  :', repr(b))
print('done', N, 'bad', bad)
