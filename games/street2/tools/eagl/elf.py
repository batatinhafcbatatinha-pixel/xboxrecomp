import struct
class Elf:
    def __init__(s,d):
        s.d=d
        (s.shoff,)=struct.unpack_from('<I',d,0x20)
        s.shentsize,s.shnum,s.shstrndx=struct.unpack_from('<HHH',d,0x2E)
        s.sh=[]
        for i in range(s.shnum):
            s.sh.append(struct.unpack_from('<10I',d,s.shoff+i*s.shentsize))
        strt=s.sh[s.shstrndx]
        s.names=[s.cstr(strt[4]+h[0]) for h in s.sh]
        s.syms=[]
        for i,h in enumerate(s.sh):
            if h[1]==2:
                st=s.sh[h[6]]
                for j in range(h[5]//16):
                    nm,val,sz,info,oth,shn=struct.unpack_from('<IIIBBH',d,h[4]+j*16)
                    s.syms.append((s.cstr(st[4]+nm),val,sz,info,shn))
        s.rels=[]
        for i,h in enumerate(s.sh):
            if h[1] in (9,4):
                ent=8 if h[1]==9 else 12
                for j in range(h[5]//ent):
                    off,info=struct.unpack_from('<II',d,h[4]+j*ent)
                    s.rels.append((h[7],off,info>>8,info&0xff))
    def cstr(s,o):
        e=s.d.index(b'\0',o); return s.d[o:e].decode('latin1')
    def sec(s,i): h=s.sh[i]; return s.d[h[4]:h[4]+h[5]]
