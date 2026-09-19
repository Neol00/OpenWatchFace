import sys,struct
board,qti,reg,out=sys.argv[1:5]
d=open(qti,'rb').read(); r=open(reg,'rb').read()
F='<HHBB'
i=d.find(struct.pack(F,2800,4,0,0x20))
if i<0:
    # search any table: entries with idx increasing from 0
    for j in range(0,len(d)-6,2):
        if struct.unpack_from(F,d,j)[3]==0 and struct.unpack_from(F,d,j+6)[3]==1 and struct.unpack_from(F,d,j+12)[3]==2 and struct.unpack_from(F,d,j)[0]==0 and struct.unpack_from(F,d,j+6)[0]==0:
            i=j; break
def ok(s): g,sz,z,ix=struct.unpack_from(F,d,s); return z==0 and sz<8192
s=i
while s-6>=0 and ok(s-6) and struct.unpack_from(F,d,s-6)[3]==struct.unpack_from(F,d,s)[3]-1: s-=6
e=i
while e+12<=len(d) and ok(e+6) and struct.unpack_from(F,d,e+6)[3]==struct.unpack_from(F,d,e)[3]+1: e+=6
tab=[struct.unpack_from(F,d,s+k*6) for k in range((e-s)//6+1)]
last=tab[-1]; assert last[3]*256+last[1]==len(r), ("layout check failed", last, len(r))
blob=bytearray(); ents=[]
for g,sz,z,ix in tab:
    off=ix*256; data=r[off:off+sz]; assert len(data)==sz
    ents.append((g,sz,len(blob))); blob+=data
o=["/* sns_reg_%s.h -- GENERATED (tools: session scratchpad gen_snsreg.py) from the %s persist backup"%(board,board),
   " * /sensors/sns.reg (%d B) and the group table in its stock vendor sensors.qti (%d groups x {group_id u16,"%(len(r),len(tab)),
   " * size u16, 0, index u8}; file offset of a group = index * 256, checked against the file size)."," * Served to the modem's sensor hub as QMI SNS_REG2 (svc 0x10f inst 2) group reads. */",
   "#define SNS_REG_NGROUPS %du"%len(ents),
   "static const struct { uint16_t id, size, off; } k_sns_groups[SNS_REG_NGROUPS] = {", ",".join("{%d,%d,%d}"%x for x in ents), "};",
   "static const uint8_t k_sns_data[%d] = {"%len(blob)]
for k in range(0,len(blob),24): o.append(",".join("0x%02x"%b for b in blob[k:k+24])+",")
o.append("};"); open(out,'w').write("\n".join(o)+"\n")
print(board,"table @%x groups %d blob %d group2800 %s"%(s,len(tab),len(blob),[blob[x[2]:x[2]+x[1]].hex() for x in ents if x[0]==2800]))
