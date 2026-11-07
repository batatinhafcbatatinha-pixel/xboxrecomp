import struct,sys
def parse(d,base=0):
    assert d[base:base+4] in (b'BIGF',b'BIG4'),d[base:base+4]
    n=struct.unpack('>I',d[base+8:base+12])[0]; p=base+16; out=[]
    for i in range(n):
        off,sz=struct.unpack('>II',d[p:p+8]); e=d.index(b'\0',p+8); out.append((d[p+8:e].decode('latin1'),base+off,sz)); p=e+1
    return out
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    ents=parse(d)
    for name,off,sz in ents:
        sub=d[off:off+4]
        print(name,hex(off),sz,sub)
        if sub==b'BIGF' and len(sys.argv)>2:
            for n2,o2,s2 in parse(d,off): print('   ',n2,s2,d[o2:o2+16].hex())
