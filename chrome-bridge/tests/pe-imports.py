"""Inspect the produced x64 PE rather than assuming static CRT settings worked."""
import json,pathlib,struct,sys
data=pathlib.Path(sys.argv[1]).read_bytes()
u16=lambda at:struct.unpack_from('<H',data,at)[0]
u32=lambda at:struct.unpack_from('<I',data,at)[0]
pe=u32(0x3c);assert data[pe:pe+4]==b'PE\0\0' and u16(pe+4)==0x8664
optional=pe+24;assert u16(optional)==0x20b
sections=optional+u16(pe+20);count=u16(pe+6)
def offset(rva):
    for index in range(count):
        entry=sections+40*index;size,base,raw_size,raw=struct.unpack_from('<IIII',data,entry+8)
        if base<=rva<base+max(size,raw_size):
            assert rva-base<raw_size;return raw+rva-base
    raise AssertionError('Import RVA is outside file sections')
directory=optional+112;assert u32(directory+13*8)==0,'Unexpected delay-import table'
imports=[];entry=offset(u32(directory+8))
while any(data[entry:entry+20]):
    name=offset(u32(entry+12));end=data.index(0,name);imports.append(data[name:end].decode('ascii'));entry+=20
allowed={'advapi32.dll','bcrypt.dll','bcryptprimitives.dll','gdi32.dll','kernel32.dll','ntdll.dll','ole32.dll','oleaut32.dll','user32.dll','userenv.dll','ws2_32.dll','ucrtbase.dll'}
unexpected=[name for name in imports if name.lower() not in allowed and not name.lower().startswith('api-ms-win-')]
assert not unexpected,unexpected
print(json.dumps({'ok':True,'architecture':'x86_64','imports':imports,'externalRedistributables':[]}))
