def decompress(s):
    p=0
    flags=s[0]; assert s[1]==0xFB,hex(s[1])
    p=2
    big = flags & 0x80
    n = 4 if big else 3
    if flags & 0x01: p+=n
    size=int.from_bytes(s[p:p+n],'big'); p+=n
    out=bytearray()
    while p<len(s):
        b0=s[p]
        if b0<0x80:
            b1=s[p+1]; p+=2
            lit=b0&3; out+=s[p:p+lit]; p+=lit
            ln=((b0>>2)&7)+3; off=((b0&0x60)<<3)+b1+1
        elif b0<0xC0:
            b1,b2=s[p+1],s[p+2]; p+=3
            lit=b1>>6; out+=s[p:p+lit]; p+=lit
            ln=(b0&0x3F)+4; off=((b1&0x3F)<<8)+b2+1
        elif b0<0xE0:
            b1,b2,b3=s[p+1],s[p+2],s[p+3]; p+=4
            lit=b0&3; out+=s[p:p+lit]; p+=lit
            ln=((b0&0x0C)<<6)+b3+5; off=((b0&0x10)<<12)+(b1<<8)+b2+1
        elif b0<0xFC:
            p+=1; lit=((b0&0x1F)<<2)+4; out+=s[p:p+lit]; p+=lit; continue
        else:
            p+=1; lit=b0&3; out+=s[p:p+lit]; p+=lit; break
        st=len(out)-off
        for i in range(ln): out.append(out[st+i])
    assert len(out)==size,(len(out),size)
    return bytes(out)
