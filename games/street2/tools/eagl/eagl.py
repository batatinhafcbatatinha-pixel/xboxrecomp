import sys,struct,collections
import os; sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import big,refpack,elf
def load_models(path):
    d=open(path,'rb').read()
    for n,o,s in big.parse(d):
        u=d[o:o+s]
        if u[:2]==b'\x10\xfb' or u[1:2]==b'\xfb': u=refpack.decompress(u)
        if u[:4]!=b'BIGF': continue
        inner=big.parse(u); names={nm:(oo,ss) for nm,oo,ss in inner}
        for nm,(oo,ss) in names.items():
            if nm.endswith('.orl'): continue
            blob=u[oo:oo+ss]
            if nm.endswith('.ord'):
                r=names.get(nm[:-1]+'l'); blob=blob+u[r[0]:r[0]+r[1]]
            yield n+'/'+nm, blob
class Obj:
    def __init__(s,blob):
        E=s.E=elf.Elf(blob)
        di=E.names.index('.data'); s.base=E.sh[di][4]; s.data=E.sec(di)
        s.rel={}  # offset -> (symname, symval, shn)
        for sec,off,si,typ in E.rels:
            sy=E.syms[si]; s.rel[off]=(sy[0],sy[1],sy[4],typ)
        s.syms={sy[0]:sy[1] for sy in E.syms if sy[4]==1}
    def u32(s,o): return struct.unpack_from('<I',s.data,o)[0]
    def ptr(s,o):
        """return (kind,value): ('int',v) / ('local',off) / ('ext',name)"""
        v=s.u32(o); r=s.rel.get(o)
        if r is None: return ('int',v)
        if r[2]==1: return ('local',v + (r[1] if r[0] else 0))
        return ('ext',r[0],v)
