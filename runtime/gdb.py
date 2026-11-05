# The gdb pretty printers for Iris programs.
#
# A program built by iris carries this script in its .debug_gdb_scripts
# section. gdb runs it when the program's folder is in "auto-load safe-path".
# "iris gdb" starts gdb with the script, and "iris gdb --printers" prints the
# path of a copy of it, for "source" in gdb.
#
# The layouts come from src/ir.c (dbgType) and std/core.

import gdb
import struct

_LIMIT = 1 << 31   # a length above this comes from memory that isn't set yet


def _tag(t):
    t = t.strip_typedefs()
    return t.tag or t.name


def _int(v):
    return int(v)


def _items(ptr, n):
    for i in range(n):
        yield "[%d]" % i, ptr[i]


def _bad_len(n):
    return n < 0 or n > _LIMIT


class TextPrinter:
    """str, String and u8[]: the bytes as text."""

    def __init__(self, v):
        self.v = v

    def to_string(self):
        n = _int(self.v["len"])
        if _bad_len(n):
            return "<text with length %d>" % n
        if n == 0:
            return ""
        return self.v["ptr"].lazy_string(encoding="utf-8", length=n)

    def display_hint(self):
        return "string"


class SlicePrinter:
    """T[]: the items."""

    def __init__(self, v):
        self.v = v
        self.n = _int(v["len"])

    def to_string(self):
        return "%s(len=%d)" % (_tag(self.v.type), self.n)

    def children(self):
        if _bad_len(self.n):
            return iter(())
        return _items(self.v["ptr"], self.n)

    def display_hint(self):
        return "array"


class ListPrinter:
    """List[T]: the first len items of buf."""

    def __init__(self, v):
        self.v = v
        self.n = _int(v["len"])

    def to_string(self):
        return "%s(len=%d)" % (_tag(self.v.type), self.n)

    def children(self):
        if _bad_len(self.n):
            return iter(())
        return _items(self.v["buf"]["ptr"], self.n)

    def display_hint(self):
        return "array"


class DequePrinter:
    """Deque[T]: len items from head, around the end of buf."""

    def __init__(self, v):
        self.v = v
        self.n = _int(v["len"])

    def to_string(self):
        return "%s(len=%d)" % (_tag(self.v.type), self.n)

    def children(self):
        buf = self.v["buf"]
        size = _int(buf["len"])
        if _bad_len(self.n) or size <= 0:
            return
        head = _int(self.v["head"])
        for i in range(self.n):
            yield "[%d]" % i, buf["ptr"][(head + i) & (size - 1)]

    def display_hint(self):
        return "array"


def _map_entries(m):
    """The (key, value) pairs of a Map: the slots whose state has bit 0x80."""
    slots = m["slots"]
    state = m["state"]
    size = _int(slots["len"])
    if _bad_len(size):
        return
    for i in range(size):
        if _int(state["ptr"][i]) & 0x80:
            e = slots["ptr"][i]
            yield e["key"], e["val"]


class MapPrinter:
    """Map[K, V]: the keys and their values."""

    def __init__(self, v):
        self.v = v

    def to_string(self):
        return "%s(len=%d)" % (_tag(self.v.type), _int(self.v["len"]))

    def children(self):
        i = 0
        for k, val in _map_entries(self.v):
            yield "[%d]" % i, k
            yield "[%d]" % (i + 1), val
            i += 2

    def display_hint(self):
        return "map"


class SetPrinter:
    """Set[T]: the keys of its map."""

    def __init__(self, v):
        self.v = v

    def to_string(self):
        return "%s(len=%d)" % (_tag(self.v.type), _int(self.v["len"]))

    def children(self):
        i = 0
        for k, _ in _map_entries(self.v["m"]):
            yield "[%d]" % i, k
            i += 1

    def display_hint(self):
        return "array"


def _read_u64(addr):
    return struct.unpack("<Q", gdb.selected_inferior().read_memory(addr, 8).tobytes())[0]


def _c_string(addr):
    if addr == 0:
        return None
    return gdb.Value(addr).cast(gdb.lookup_type("char").pointer()).string()


def error_code_text(code):
    """An error code as Enum.value, from the table that the compiler writes."""
    code &= (1 << 64) - 1
    kind = code >> 32
    value = code & 0xFFFFFFFF
    if value >= 1 << 31:
        value -= 1 << 32
    try:
        table = int(gdb.parse_and_eval("(long)&iris_enum_table"))
        entry = table + 32 * (kind - 1)
        enum_name = _c_string(_read_u64(entry))
        count = _read_u64(entry + 8)
        values = _read_u64(entry + 16)
        names = _read_u64(entry + 24)
        for i in range(min(count, 4096)):
            v = struct.unpack("<q", gdb.selected_inferior().read_memory(values + 8 * i, 8).tobytes())[0]
            if v == value:
                return "%s.%s" % (enum_name, _c_string(_read_u64(names + 8 * i)))
        return "%s.%d" % (enum_name, value)
    except (gdb.error, gdb.MemoryError, ValueError):
        return "error code %d" % code


class ErrorPrinter:
    """Error and the code of an error: Enum.value."""

    def __init__(self, v):
        self.v = v

    def to_string(self):
        return error_code_text(_int(self.v["code"]))


class ResultPrinter:
    """!T: the value, or the error."""

    def __init__(self, v):
        self.v = v

    def to_string(self):
        code = _int(self.v["error"]["code"])
        if code != 0:
            return "error(%s)" % error_code_text(code)
        fields = [f.name for f in self.v.type.strip_typedefs().fields()]
        if "value" not in fields:
            return "ok"
        return self.v["value"]


class DataEnumPrinter:
    """An enum with data: Enum.Member and the member's fields."""

    def __init__(self, v):
        self.v = v
        self.member = str(v["tag"])
        names = [f.name for f in v["data"].type.strip_typedefs().fields()]
        self.data = v["data"][self.member] if self.member in names else None

    def to_string(self):
        return "%s.%s" % (_tag(self.v.type), self.member)

    def children(self):
        if self.data is None:
            return
        for f in self.data.type.strip_typedefs().fields():
            yield f.name, self.data[f.name]


def _is_data_enum(t):
    fields = t.fields()
    return (len(fields) == 2 and fields[0].name == "tag" and fields[1].name == "data"
            and fields[0].type.strip_typedefs().code == gdb.TYPE_CODE_ENUM
            and fields[1].type.strip_typedefs().code == gdb.TYPE_CODE_UNION)


def iris_lookup(v):
    t = v.type.strip_typedefs()
    if t.code != gdb.TYPE_CODE_STRUCT:
        return None
    name = t.tag or t.name
    if not name:
        return None
    if name in ("str", "String"):
        return TextPrinter(v)
    if name in ("Error", "ErrorCode"):
        return ErrorPrinter(v)
    if name.startswith("!"):
        return ResultPrinter(v)
    if name.endswith("[]"):
        return SlicePrinter(v)
    if name.startswith("List["):
        return ListPrinter(v)
    if name.startswith("Map["):
        return MapPrinter(v)
    if name.startswith("Set["):
        return SetPrinter(v)
    if name.startswith("Deque["):
        return DequePrinter(v)
    if _is_data_enum(t):
        return DataEnumPrinter(v)
    return None


# Each program can load the script again: it adds the printers once.
if not any(getattr(p, "__name__", "") == "iris_lookup" for p in gdb.pretty_printers):
    gdb.pretty_printers.append(iris_lookup)
