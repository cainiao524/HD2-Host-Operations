"""Read-only access to a HELLDIVERS 2 data folder, both layouts:

  slim  (2025+): bundles.nxa (DSAR -> DSAA archive table) + bundles.NN.nxa chunk stores, plus some single-archive DSAR
                 files named by their archive hash.
  fat   (2024):  one stingray archive per file (<16 hex>), with <name>.gpu_resources and <name>.stream beside it.

Both carry the same stingray archive header (magic 0xF0000011, 32-byte type rows, 80-byte file rows), as FileDiver
reads them (stingray/archive.go, slim_edition.go, fat_edition.go).

  d = DataDir(path)          # or DataDir(path, files=[...]) for loose archives such as a mod's *.patch_0
  d.index                    # {(type, name): [Entry]}, both 16-hex strings
  d.read(type, name, part)   # part 0 main, 1 stream, 2 gpu; the first archive that has it
"""
import bisect, collections, os, re, struct
import lz4.block

HEX16 = re.compile(r"^[0-9a-f]{16}$")
Entry = collections.namedtuple("Entry", "archive offsets sizes")


def murmur64(data, seed=0):
    m, mask = 0xC6A4A7935BD1E995, (1 << 64) - 1
    h = (seed ^ (len(data) * m)) & mask
    n = len(data) // 8
    for i in range(n):
        k = int.from_bytes(data[i * 8:i * 8 + 8], "little")
        k = (k * m) & mask; k ^= k >> 47; k = (k * m) & mask
        h ^= k; h = (h * m) & mask
    tail = data[n * 8:]
    if tail:
        h ^= int.from_bytes(tail, "little"); h = (h * m) & mask
    h ^= h >> 47; h = (h * m) & mask; h ^= h >> 47
    return h


def h64(name):
    return f"{murmur64(name.encode()):016x}"


TYPE_NAMES = ("unit level package material texture bones animation state_machine physics particles wwise_bank "
              "wwise_stream wwise_dep strings prefab entity geometry_group render_config font lua runtime_font ah_bin "
              "behavior_tree network_config speedtree camera_shake cloth hash_lookup renderable shading_environment "
              "shading_environment_mapping vector_field wwise_metadata wwise_properties bik config mouse_cursor "
              "shader shader_library shader_library_group surface_properties texture_atlas timpani_bank "
              "timpani_master apb data level_stream").split()
TYPES = {h64(t): t for t in TYPE_NAMES}
TYPE = {t: h for h, t in TYPES.items()}
assert TYPE["unit"] == "e0a48d0be9a7453f"


def parse_archive_head(buf):
    """buf(n) returns the archive's first n bytes. Returns [(type, name, offsets[3], sizes[3])]."""
    head = buf(72)
    magic, nt, nf = struct.unpack_from("<III", head, 0)
    if magic != 0xF0000011:
        raise ValueError(f"not a stingray archive (magic {magic:#x})")
    data = buf(72 + nt * 32 + nf * 80)
    base = 72 + nt * 32
    out = []
    for i in range(nf):
        o = base + i * 80
        name, typ = struct.unpack_from("<QQ", data, o)
        offsets = struct.unpack_from("<QQQ", data, o + 16)
        sizes = struct.unpack_from("<III", data, o + 56)
        out.append((f"{typ:016x}", f"{name:016x}", offsets, sizes))
    return out


class _Dsar:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            magic, _, n, _, _ = struct.unpack("<4s4sIIQ", f.read(24)); f.read(8)
            if magic != b"DSAR":
                raise ValueError(f"{path}: not DSAR")
            raw = f.read(n * 32)
        self.chunks = [struct.unpack_from("<QQIIBB", raw, i * 32) for i in range(n)]
        self.starts = [c[0] for c in self.chunks]
        self.f = None

    def chunk(self, k):
        if self.f is None:
            self.f = open(self.path, "rb")
        c = self.chunks[k]
        self.f.seek(c[1])
        d = self.f.read(c[3])
        return d if c[4] == 0 else lz4.block.decompress(d, uncompressed_size=c[2])

    def read(self, uoff, n):
        """n bytes from uncompressed offset uoff."""
        k = bisect.bisect_right(self.starts, uoff) - 1
        out = bytearray()
        skip = uoff - self.starts[k]
        while len(out) < skip + n and k < len(self.chunks):
            out += self.chunk(k); k += 1
        return bytes(out[skip:skip + n])


class DataDir:
    def __init__(self, path, files=None):
        self.path = path
        self.slim = files is None and os.path.exists(os.path.join(path, "bundles.nxa"))
        self.index = collections.defaultdict(list)
        self._readers = {}  # (archive, part) -> read(offset, n)
        if self.slim:
            self._open_slim()
        else:
            self._open_fat(files)

    # ---- fat -------------------------------------------------------------------------------------------------------
    def _open_fat(self, files):
        names = files if files is not None else [n for n in os.listdir(self.path) if HEX16.match(n)]
        for name in names:
            full = name if os.path.isabs(name) else os.path.join(self.path, name)
            if not os.path.isfile(full):
                continue
            archive = os.path.basename(full)
            with open(full, "rb") as f:
                try:
                    rows = parse_archive_head(lambda n: (f.seek(0), f.read(n))[1])
                except ValueError:
                    continue
            for part, ext in enumerate(("", ".stream", ".gpu_resources")):
                p = full + ext
                if os.path.isfile(p):
                    self._readers[(archive, part)] = self._file_reader(p)
            self._add(archive, rows)

    @staticmethod
    def _file_reader(p):
        def read(off, n):
            with open(p, "rb") as f:
                f.seek(off); return f.read(n)
        return read

    # ---- slim ------------------------------------------------------------------------------------------------------
    def _open_slim(self):
        top = _Dsar(os.path.join(self.path, "bundles.nxa"))
        b = b"".join(top.chunk(k) for k in range(len(top.chunks)))
        magic, _, _, nname, nar = struct.unpack_from("<4sIIII", b, 0)
        assert magic == b"DSAA"
        cstr = lambda o: b[o:b.index(b"\0", o)].decode()
        bundles = [_Dsar(os.path.join(self.path, cstr(struct.unpack_from("<I", b, 24 + nar * 24 + i * 4)[0])))
                   for i in range(nname)]
        for i in range(nar):
            size, fno, ecount, eoff = struct.unpack_from("<QIIQ", b, 24 + i * 24)
            fn = cstr(fno)
            stem, _, ext = fn.partition(".")
            part = {"": 0, "stream": 1, "gpu_resources": 2}.get(ext)
            if part is None:
                continue
            ents = [struct.unpack_from("<I4xI3xB", b, eoff + j * 16) for j in range(ecount)]
            self._readers[(stem, part)] = self._slim_reader(bundles, ents)
        for (stem, part), read in list(self._readers.items()):
            if part == 0:
                try:
                    self._add(stem, parse_archive_head(lambda n, read=read: read(0, n)))
                except Exception as e:  # noqa: BLE001 - report and keep going
                    print("skip", stem, e)
        for name in os.listdir(self.path):
            stem, _, ext = name.partition(".")
            if not HEX16.match(stem) or (stem, 0) in self._readers:
                continue
            part = {"": 0, "stream": 1, "gpu_resources": 2}.get(ext)
            if part is None:
                continue
            ds = _Dsar(os.path.join(self.path, name))
            self._readers[(stem, part)] = ds.read
            if part == 0:
                self._add(stem, parse_archive_head(lambda n, ds=ds: ds.read(0, n)))

    @staticmethod
    def _slim_reader(bundles, ents):
        aoffs = [e[0] for e in ents]
        def read(off, n):
            i = bisect.bisect_right(aoffs, off) - 1
            aoff, uoff, bi = ents[i]
            return bundles[bi].read(uoff + (off - aoff), n)
        return read

    # ---- common ----------------------------------------------------------------------------------------------------
    def _add(self, archive, rows):
        for typ, name, offsets, sizes in rows:
            self.index[(typ, name)].append(Entry(archive, offsets, sizes))

    def read(self, typ, name, part=0, entry=None):
        for e in ([entry] if entry else self.index.get((typ, name), [])):
            if e.sizes[part] == 0:
                return b""
            r = self._readers.get((e.archive, part))
            if r:
                return r(e.offsets[part], e.sizes[part])
        return None

    def names(self):
        return {n for (_, n) in self.index}


def load_names(paths):
    names = {}
    for p in paths:
        with open(p, encoding="utf-8", errors="ignore") as f:
            for line in f:
                s = line.strip()
                if s:
                    names[h64(s)] = s
    return names


def package_items(data):
    """A package resource: 16-byte header (magic, ?, count, ?) then count x (type u64, name u64)."""
    if data is None or len(data) < 16:
        return []
    _, _, count, _ = struct.unpack_from("<IIII", data, 0)
    if count > (len(data) - 16) // 16:
        return []
    return [(f"{t:016x}", f"{n:016x}") for t, n in struct.iter_unpack("<QQ", data[16:16 + count * 16])]
