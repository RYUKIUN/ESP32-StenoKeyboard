import sys, json, logging
logging.disable(logging.CRITICAL)
from plover import system
from plover.registry import registry
registry.update()
system.setup('English Stenotype')
from plover.steno import Stroke, normalize_steno
from plover.translation import Translator
from plover.formatting import Formatter
from plover.steno_dictionary import StenoDictionary, StenoDictionaryCollection

import os; sys.path.insert(0, os.path.join(os.path.dirname(__file__),'..')); from convert_dict import sanitize
def make(dictpath_list):
    d = StenoDictionary()
    for p in dictpath_list:
        for k, v in json.load(open(p)).items():
            v = sanitize(v)
            if v is not None: d[normalize_steno(k)] = v
    return d

class Out:
    def __init__(s): s.buf=[]
    def send_backspaces(s,n):
        for _ in range(n):
            if s.buf: s.buf.pop()
    def send_string(s,t): s.buf.extend(t)
    def send_key_combination(s,c): s.buf.extend('<%s>'%c.replace(' ',''))
    def send_engine_command(s,c): pass

def run(d, strokes):
    t = Translator(); coll = StenoDictionaryCollection([d]); t.set_dictionary(coll)
    f = Formatter(); o = Out(); f.set_output(o); t.add_listener(f.format)
    f.set_space_placement('Before Output')
    for s in strokes: t.translate(Stroke(s) if not isinstance(s,Stroke) else s)
    return ''.join(o.buf)
