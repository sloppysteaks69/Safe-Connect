"""Make safe-connect.exe from a PyInstaller 6 onefile exe built with Python 3.12 and
tkinter (Prospero Sender Turbo 6.49's): keeps its bootloader, Python and Tcl/Tk,
keeps only the Python modules and files this app uses, and swaps in the app.

usage: python3.12 make_exe.py BASE.exe OUT.exe safe_connect.py [icon-256.png]
The icon step needs pefile and Pillow."""
import marshal, struct, sys, zlib, importlib.util
COOKIE = b'MEI\014\013\012\013\016'; CF = '!8sIIII64s'; CL = struct.calcsize(CF)
TF = '!IIIIBc'; TL = struct.calcsize(TF)

# Python modules the app loads (found by running it), plus the PyInstaller bootstrap's.
KEEP_MODULES = {
    'base64', 'bisect', 'collections', 'contextlib', 'copyreg', 'ctypes', 'enum', 'fnmatch', 'ftplib',
    'functools', 'heapq', 'importlib', 'ipaddress', 'json', 'keyword', 'ntpath', 'operator', 'pathlib',
    'pkgutil', 'queue', 're', 'reprlib', 'selectors', 'socket', 'stringprep', 'struct', 'threading',
    'tkinter', 'types', 'typing', 'urllib', 'warnings', 'weakref', 'string', 'random', 'zipimport',
    'tracemalloc', 'linecache', 'tokenize', 'token', 'textwrap', 'inspect', 'dis', 'opcode', 'ast',
    'stat', 'genericpath', 'posixpath', 'os', 'io', 'abc', 'codecs', 'locale', 'subprocess', 'signal',
    'datetime', '_pydatetime', '_strptime', 'calendar', 'numbers', 'copy', 'shutil', 'tempfile',
    'zipfile', '_compression', 'gzip', 'bz2', 'lzma', 'hashlib', 'glob', 'email', 'quopri',
    'contextvars', 'dataclasses', 'traceback', 'nturl2path', 'pickle', '_compat_pickle', 'pprint',
    'sre_compile', 'sre_parse', 'sre_constants', 'winsound', '_threading_local', 'atexit',
    'encodings', '__future__', 'ftplib', 'shutil', 'traceback', 'zipfile', 'winreg',
}
KEEP_SCRIPTS = {'pyiboot01_bootstrap', 'pyi_rth_pkgutil', 'pyi_rth__tkinter', 'pyi_rth_inspect'}
KEEP_BINARIES = {'VCRUNTIME140.dll', 'python312.dll', 'base_library.zip', '_ctypes.pyd', 'libffi-8.dll',
                 '_socket.pyd', 'select.pyd', '_queue.pyd', '_tkinter.pyd', 'tcl86t.dll', 'tk86t.dll',
                 'unicodedata.pyd', 'winsound.pyd'}
TCL_ENCODINGS = {'cp1252.enc', 'cp437.enc', 'cp850.enc', 'iso8859-1.enc', 'ascii.enc'}


def keep_file(name):
    parts = name.split('\\')
    if len(parts) == 1:
        return name in KEEP_BINARIES
    top = parts[0]
    if top == 'sounds':
        return True
    if top == '_tcl_data':
        if parts[1] in ('tzdata', 'msgs'):
            return False
        if parts[1] == 'encoding':
            return parts[2] in TCL_ENCODINGS
        return True
    if top == '_tk_data':
        return parts[1] not in ('images', 'demos')
    return top == 'tcl8'


old, new, script = sys.argv[1:4]
assert sys.version_info[:2] == (3, 12)
data = open(old, 'rb').read(); at = data.rfind(COOKIE)
magic, alen, toc_off, toc_len, pyvers, pylib = struct.unpack_from(CF, data, at); assert pyvers == 312
start = at + CL - alen; boot = data[:start]; arch = data[start:at + CL]
toc = []; pos = toc_off
while pos < toc_off + toc_len:
    elen, off, clen, ulen, comp, tc = struct.unpack_from(TF, arch, pos)
    toc.append((off, clen, ulen, comp, tc, arch[pos + TL:pos + elen].rstrip(b'\0').decode())); pos += elen
out = bytearray(); ents = []; swapped = False
for off, clen, ulen, comp, tc, name in toc:
    blob = arch[off:off + clen]
    if tc == b's':
        if name in KEEP_SCRIPTS:
            pass
        elif name == 'pw_gui':      # the base exe's app script, run after the runtime hooks
            src = open(script, encoding='utf-8').read()
            raw = marshal.dumps(compile(src, 'safe_connect.py', 'exec'))
            blob, ulen, comp, name = zlib.compress(raw, 9), len(raw), 1, 'safe_connect'; swapped = True
        else:
            continue
    elif tc in (b'b', b'x') and not keep_file(name):
        continue
    elif tc == b'z':
        assert blob[:4] == b'PYZ\0' and blob[4:8] == importlib.util.MAGIC_NUMBER
        (ptoc_off,) = struct.unpack_from('!i', blob, 8)
        body = bytearray(blob[:17]); nt = []
        for pname, (ptc, poff, plen) in marshal.loads(blob[ptoc_off:]):
            if pname.split('.')[0] in KEEP_MODULES and '.test' not in pname and 'idle' not in pname:
                nt.append((pname, (ptc, len(body), plen))); body += blob[poff:poff + plen]
        struct.pack_into('!i', body, 8, len(body)); body += marshal.dumps(nt)
        blob = bytes(body); ulen = clen = len(blob)
    ents.append((len(out), len(blob), ulen, comp, tc, name)); out += blob
assert swapped
tb = b''
for off, clen, ulen, comp, tc, name in ents:
    nb = name.encode(); nl = len(nb) + 1
    if (TL + nl) % 16: nl += 16 - (TL + nl) % 16
    tb += struct.pack(TF + f'{nl}s', TL + nl, off, clen, ulen, comp, tc, nb)
toc_offset = len(out); out += tb
out += struct.pack(CF, COOKIE, toc_offset + len(tb) + CL, toc_offset, len(tb), 312, pylib)
open(new, 'wb').write(boot + bytes(out)); print('ok', len(boot) + len(out), len(ents), 'entries')


# The exe's icon: the base exe's icon images are overwritten in place with PNG
# images of icon-256.png (the icon the window shows), so nothing else moves.
def set_icon(exe, png):
    import io, struct as st, pefile
    from PIL import Image
    pe = pefile.PE(exe)
    data = bytearray(open(exe, 'rb').read())
    big = Image.open(png).convert('RGBA')
    icons = {}; group = None
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        for e in t.directory.entries:
            for l in e.directory.entries:
                if t.id == 3: icons[e.id] = l.data.struct
                if t.id == 14: group = l.data.struct
    goff = pe.get_offset_from_rva(group.OffsetToData)
    _r, _t, count = st.unpack_from('<HHH', data, goff)
    for i in range(count):
        at = goff + 6 + i * 14
        w, h, colours, _r2, planes, bits, size, ident = st.unpack_from('<BBBBHHIH', data, at)
        px = w or 256
        buf = io.BytesIO(); big.resize((px, px), Image.LANCZOS).save(buf, 'PNG', optimize=True)
        img = buf.getvalue(); entry = icons[ident]
        assert len(img) <= entry.Size, (ident, len(img), entry.Size)
        off = pe.get_offset_from_rva(entry.OffsetToData)
        data[off:off + entry.Size] = img + bytes(entry.Size - len(img))
        st.pack_into('<I', data, entry.get_file_offset() + 4, len(img))
        st.pack_into('<BBBBHHIH', data, at, w, h, 0, 0, 1, 32, len(img), ident)
    pe.close()
    open(exe, 'wb').write(bytes(data))


if len(sys.argv) > 4:
    set_icon(new, sys.argv[4]); print('icon set')
