import sys,os,glob,collections
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import eagl
root=sys.argv[1]
ops=collections.Counter(); where=collections.defaultdict(list); symkinds=collections.Counter(); nobj=0
for f in glob.glob(root+'/**/*.big',recursive=True):
    try: items=list(eagl.load_models(f))
    except Exception as e: print('skip',f,e); continue
    for name,blob in items:
        if blob[:4]!=b'\x7fELF': continue
        try: o=eagl.Obj(blob)
        except Exception as e: print('bad',name,e); continue
        nobj+=1
        for k in o.syms: symkinds[k.split(':::')[0]]+=1
        for k,v in o.syms.items():
            if not k.startswith('__RM'): continue
            st=o.ptr(v)
            if st[0]!='local': continue
            p=st[1]
            for _ in range(10000):
                h=o.u32(p)
                if h==0: break
                op,sz=h>>16,h&0xffff
                ops[op]+=1
                if len(where[op])<3: where[op].append((os.path.basename(f),name,hex(p)))
                if sz==0: print('zero size',name); break
                p+=sz*4
print('objects',nobj)
print(sorted(ops.items()))
for op in sorted(where): print(op,where[op])
print(symkinds.most_common(40))
