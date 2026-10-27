#!/usr/bin/env python3
"""Pack Jura machine XMLs into the compact JPRF database used by the jura_link
ESPHome component (components/jura_link/jura_profiles_blob.h).

Only the fields the dongle needs are kept (products + brew options, maintenance
labels, alerts, model family), with a shared string pool. No XML parser or
inflate is needed on the ESP32.

Usage:
  python3 tools/pack_profiles.py --xml-dir <dir> [--machines JOE_MACHINES.TXT]
         [--header components/jura_link/jura_profiles_blob.h] [--bin out.jprf]

<dir> holds one folder per EF code with the machine XML inside
(e.g. <dir>/EF536/1.0.xml); the folder name is the key the machine reports.
JOE_MACHINES.TXT (article;name;EF code;...) supplies model family names; without
it, machines are named by their EF code.

Layout (little-endian):
  header:  magic 'JPRF', u16 version, u16 machine_count, u32 strpool_off
  machine index: machine_count * { u16 ef_str, u16 name_str, u32 rec_off }   (version 4)
                 name_str = model family from JOE_MACHINES.TXT (e.g. EF536 -> "S8"), "" if unknown
  per-machine record:
     u8  n_products
     products[]: u8 code, u16 name_str, u8 flags(bit0 active, bit1 double), u8 n_opts,
                 opts[]: u8 attr, u8 arg, u16 def, u16 min, u16 max, u16 step, u8 n_items,
                         items[]: u8 value, u16 name_str
       attr: 0 coffee_strength, 1 grinder_ratio, 2 temperature, 3 water_amount,
             4 milk_amount, 5 milk_foam_amount, 6 bypass, 7 milk_break  (jura-mqtt Attr order)
       arg:  n of Argument="F<n>" = byte index in the BLE brew frame; @TP: blob index = n-1
       pick lists (n_items>0): def = default ITEM value, min/max/step unused
       ranges (n_items==0):    def/min/max/step in the XML's units (ml or s)
     u8  n_maint_ctr; maint_ctr[]: u16 label_str
     u8  n_maint_pct; maint_pct[]: u16 label_str
     u16 n_alerts; alerts[]: u16 bit (bit15=blocking), u16 name_str
  string pool: concatenated NUL-terminated UTF-8, referenced by u16 offset
"""
import re, glob, struct, os, zlib, sys

XML_DIR = None        # set from --xml-dir
MACHINES_TXT = None   # set from --machines (optional)

def family(n):
    """'S8 (EA)' / 'S8 60Hz' / 'JURA WE8' -> 'S8' / 'S8' / 'WE8' (region/frequency/brand dropped)."""
    n=re.sub(r'\s*\([^)]*\)','',n); n=re.sub(r'\s*\d+\s*Hz\b','',n,flags=re.I); n=re.sub(r'^JURA\s+','',n,flags=re.I)
    return n.strip()

def load_families():
    """EF code (upper-case) -> model family, or absent when unknown/ambiguous.
    One EF code covers many articles (EF536: S8, S8 (EA), S80, S808 ...); the
    serial link only reports the EF code, so use the most common family and
    give up on a tie."""
    import collections
    if not MACHINES_TXT or not os.path.exists(MACHINES_TXT): return {}
    g=collections.defaultdict(list)
    for l in open(MACHINES_TXT,encoding='utf-8',errors='replace').read().splitlines()[1:]:
        p=l.split(';')
        if len(p)>=3 and p[1].strip(): g[p[2].strip().upper()].append(family(p[1].strip()))
    out={}
    for ef,names in g.items():
        top=collections.Counter(names).most_common(2)
        if len(top)==1 or top[0][1]>top[1][1]: out[ef]=top[0][0]
    return out

def family_for(fam, ef):
    ef=ef.upper()
    for cand in (ef, re.sub(r'V\d+$','',ef), re.sub(r'_C$','',ef), re.sub(r'(COFFEEONLY|PP|UL|ULV\d+)$','',ef)):
        if cand in fam: return fam[cand]
    return ""

class Strings:
    def __init__(self): self.buf=bytearray(); self.map={}
    def ref(self,s):
        s=s or ""
        if s in self.map: return self.map[s]
        off=len(self.buf)
        if off>0xFFFF: raise SystemExit("string pool >64KB, need u32 refs")
        self.map[s]=off; self.buf+=s.encode('utf-8',errors='replace')+b'\x00'
        return off

def attrs(x): return dict(re.findall(r'(\w+)="([^"]*)"',x))
def i8(v,d=0):
    try: return int(v)&0xFF
    except: return d
def h8(v,d=0):
    try: return int(v,16)&0xFF
    except: return d

ATTRS=['COFFEE_STRENGTH','GRINDER_RATIO','TEMPERATURE','WATER_AMOUNT',
       'MILK_AMOUNT','MILK_FOAM_AMOUNT','BYPASS','MILK_BREAK']

def u16(v,d=0):
    try: return max(0,min(0xFFFF,int(v)))
    except: return d

def parse_opts(body):
    """Brew options with a plain Argument="F<n>" (sub-byte forms like F14_1 / F2_H
    are skipped, as jura-mqtt does)."""
    opts=[]
    for om in re.finditer(r'<([A-Z_]+)\b([^>]*?)(?:/>|>(.*?)</\1>)',body,re.S):
        tag=om.group(1)
        if tag not in ATTRS: continue
        a=attrs(om.group(2))
        am=re.fullmatch(r'F(\d+)',a.get('Argument',''))
        if not am or not 1<=int(am.group(1))<=16: continue
        items=[(h8(it.get('Value')),it.get('Name','')) for it in
               (attrs(x) for x in re.findall(r'<ITEM\b([^>]*)',om.group(3) or ''))]
        if items:
            opts.append((ATTRS.index(tag),int(am.group(1)),h8(a.get('Default'),items[0][0]),0,0,0,items))
        elif 'Value' in a:
            v=u16(a['Value']); opts.append((ATTRS.index(tag),int(am.group(1)),v,
                u16(a.get('Min'),v),u16(a.get('Max'),v),max(1,u16(a.get('Step'),1)),[]))
    return opts

def parse(xml):
    m={'ef':'','products':[],'mc':[],'mp':[],'alerts':[]}
    r=re.search(r'<root\b([^>]*)>',xml); a=attrs(r.group(1)) if r else {}
    m['ef']=a.get('EF_code') or a.get('EFCode') or ''
    # Products are either <PRODUCT ...>...</PRODUCT> or self-closing <PRODUCT .../>.
    for pm in re.finditer(r'<PRODUCT\b([^>]*?)(?:/>|>(.*?)</PRODUCT>)',xml,re.S):
        pa=attrs(pm.group(1)); body=pm.group(2) or ''
        def sub(tag,k,hexv=False):
            mm=re.search(r'<'+tag+r'\b([^>]*)',body)
            if not mm: return None
            d=attrs(mm.group(1)); return d.get(k)
        p={'code':int(pa['Code'],16),'name':pa.get('Name','').strip(),
           'active':pa.get('Active','true')!='false','double':pa.get('DoubleProduct')=='true',
           'opts':parse_opts(body)}
        m['products'].append(p)
    mp=re.search(r'<MAINTENANCEPAGE\b.*?</MAINTENANCEPAGE>',xml,re.S)
    if mp:
        for bank in re.finditer(r'<BANK\b([^>]*)>(.*?)</BANK>',mp.group(0),re.S):
            cmd=attrs(bank.group(1)).get('Command','')
            items=[attrs(t.group(1)).get('Type','?') for t in re.finditer(r'<TEXTITEM\b([^>]*)',bank.group(2))]
            if cmd=='@TG:43': m['mc']=items
            elif cmd=='@TG:C0': m['mp']=items
    for al in re.finditer(r'<ALERT\b([^>]*)',xml):
        d=attrs(al.group(1))
        # bit position + a human name if present
        bit=d.get('Bit') or d.get('BitNr') or d.get('Position') or '0'
        nm=d.get('Name') or ''
        try: b=int(bit)&0x7FFF
        except: b=0
        if d.get('Type','').lower()=='block': b|=0x8000   # bit15 = blocking alert
        m['alerts'].append((b,nm))
    return m

def pack(include_alerts=True):
    S=Strings(); recs=[]; index=[]; fam=load_families()
    files=sorted(glob.glob(os.path.join(XML_DIR,'*','*.xml')))
    for f in files:
        m=parse(open(f,encoding='utf-8',errors='replace').read())
        m['ef']=os.path.basename(os.path.dirname(f))   # dir name is the EF### key (matches TY:)
        rec=bytearray()
        prods=[p for p in m['products'] if p['name']]
        rec+=struct.pack('<B',len(prods))
        for p in prods:
            fl=(1 if p['active'] else 0)|(2 if p['double'] else 0)
            rec+=struct.pack('<BHBB',p['code'],S.ref(p['name']),fl,len(p['opts']))
            for at,arg,d,mn,mx,st,items in p['opts']:
                rec+=struct.pack('<BBHHHHB',at,arg,d,mn,mx,st,len(items))
                for v,n in items: rec+=struct.pack('<BH',v,S.ref(n))
        rec+=struct.pack('<B',len(m['mc']))+b''.join(struct.pack('<H',S.ref(x)) for x in m['mc'])
        rec+=struct.pack('<B',len(m['mp']))+b''.join(struct.pack('<H',S.ref(x)) for x in m['mp'])
        if include_alerts:
            al=m['alerts']; rec+=struct.pack('<H',len(al))+b''.join(struct.pack('<HH',b,S.ref(n)) for b,n in al)
        else:
            rec+=struct.pack('<H',0)
        index.append((S.ref(m['ef']),S.ref(family_for(fam,m['ef'])))); recs.append(bytes(rec))
    # assemble
    body=bytearray(); offs=[]
    for r in recs: offs.append(len(body)); body+=r
    out=bytearray(); out+=b'JPRF'+struct.pack('<HHI',4,len(recs),0)
    idx=bytearray()
    base=len(out)+len(recs)*8
    for (efstr,namestr),o in zip(index,offs): idx+=struct.pack('<HHI',efstr,namestr,base+o)
    out+=idx+body
    strpool_off=len(out); out+=S.buf
    struct.pack_into('<I',out,8,strpool_off)
    return bytes(out),len(files),len(S.buf),len(S.map)

def main():
    import argparse
    global XML_DIR, MACHINES_TXT
    root=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap=argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--xml-dir",required=True,help="folder with one <EFcode>/<version>.xml per machine")
    ap.add_argument("--machines",help="JOE_MACHINES.TXT for model family names (optional)")
    ap.add_argument("--header",default=os.path.join(root,"components","jura_link","jura_profiles_blob.h"),
                    help="C header to write (default: the component's jura_profiles_blob.h)")
    ap.add_argument("--bin",help="also write the raw .jprf blob here")
    a=ap.parse_args()
    XML_DIR=a.xml_dir; MACHINES_TXT=a.machines
    if not glob.glob(os.path.join(XML_DIR,'*','*.xml')): sys.exit("no <EFcode>/*.xml files under %s"%XML_DIR)
    blob,n,spool,nstr=pack(True)
    print("%d machines | blob=%d B | strpool=%d B (%d unique strings) | gzip=%d B"%(n,len(blob),spool,nstr,len(zlib.compress(blob,9))))
    if a.bin: open(a.bin,'wb').write(blob)
    h=["// Auto-generated by tools/pack_profiles.py -- packed JPRF machine profiles.","#pragma once","#include <cstdint>","",
       "static const uint8_t JURA_PROFILES[] = {"]
    for i in range(0,len(blob),16):
        h.append("  "+ ",".join("0x%02x"%b for b in blob[i:i+16])+",")
    h+=["};","static const unsigned JURA_PROFILES_LEN = %d;"%len(blob),""]
    open(a.header,'w').write("\n".join(h))
    print("wrote",a.header)

if __name__=="__main__":
    main()
