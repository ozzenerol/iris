# The gdb pretty printers for Iris programs.
#
# A program built by iris carries this script in its .debug_gdb_scripts
# section. gdb runs it when the program's folder is in "auto-load safe-path".
# "iris gdb" starts gdb with the script, and "iris gdb --printers" prints the
# path of a copy of it, for "source" in gdb.
#
# The layouts come from src/ir.c (dbgType) and std/core. The task list comes
# from runtime/rt.c: allScheds, each Sched's tasks and each Task's saved sp.
#
# Commands:
#   iris tasks            one line for each task of the program
#   iris tasks --json     the same as JSON, for tools
#   iris task N [CMD]     runs CMD (default: backtrace) on the stack of task N
#   iris finish           finish, with the value of a List, String, tuple or struct read right
#
# MI command for tools:
#   -iris-return-value F  after -exec-finish: the value that function F returned
#
# Functions for breakpoint conditions on text (str, String, u8[] or "text"):
#   $iris_eq(a, b), $iris_contains(s, part), $iris_starts(s, start), $iris_ends(s, end)

import json
import os
import re
import gdb
import struct

_LIMIT = 1 << 32   # a length or a cap above this comes from memory that isn't set yet


def _tag(t):
    t = t.strip_typedefs()
    return t.tag or t.name


def _int(v):
    return int(v)


def _max_items():
    """How many items a printer reads: gdb's "set print elements"."""
    n = gdb.parameter("print elements")
    return _LIMIT if n is None or n <= 0 else n


def _readable(addr, size=1):
    try:
        gdb.selected_inferior().read_memory(addr, size)
        return True
    except (gdb.MemoryError, gdb.error, OverflowError, ValueError):
        return False


def _slice_ok(sl, cap=None):
    """A { ptr, len } that can be real: 0 <= len <= _LIMIT (and <= cap), and a
    pointer that is not null when len > 0."""
    try:
        n = _int(sl["len"])
        ptr = _int(sl["ptr"])
    except (gdb.MemoryError, gdb.error):
        return False
    if n < 0 or n > _LIMIT or (cap is not None and n > cap):
        return False
    return n == 0 or ptr != 0


def _items(ptr, n):
    """The first n items at ptr, at most "print elements" of them. An item
    that gdb cannot read shows as <unreadable>, and the items stop there."""
    for i in range(min(n, _max_items())):
        try:
            v = ptr[i]
            v.fetch_lazy()
        except (gdb.MemoryError, gdb.error):
            yield "[%d]" % i, "<unreadable>"
            return
        yield "[%d]" % i, v


def _not_set(v):
    return "%s(not set)" % _tag(v.type)


def _bad_len(n):
    return n < 0 or n > _LIMIT


class TextPrinter:
    """str, String and u8[]: the bytes as text."""

    def __init__(self, v):
        self.v = v
        self.n = 0
        self.why = None
        names = [f.name for f in v.type.strip_typedefs().fields()]
        cap = None
        try:
            if "cap" in names:
                cap = _int(v["cap"])
                if cap < 0 or cap > _LIMIT:
                    self.why = "not set"
            if self.why is None and not _slice_ok(v, cap):
                self.why = "not set"
            if self.why is None:
                self.n = _int(v["len"])
                ptr = _int(v["ptr"])
                if self.n > 0 and not _readable(ptr):
                    self.why = "unreadable"
        except (gdb.MemoryError, gdb.error):
            self.why = "unreadable"

    def to_string(self):
        if self.why:
            return "%s(%s)" % (_tag(self.v.type), self.why)
        if self.n == 0:
            return ""
        # gdb reads at most "print elements" bytes of it
        if not _readable(_int(self.v["ptr"]) + min(self.n, _max_items()) - 1):
            return "%s(unreadable)" % _tag(self.v.type)
        return self.v["ptr"].lazy_string(encoding="utf-8", length=self.n)

    def display_hint(self):
        if self.why or (self.n and not _readable(_int(self.v["ptr"]) + min(self.n, _max_items()) - 1)):
            return None
        return "string"


class SlicePrinter:
    """T[]: the items."""

    def __init__(self, v):
        self.v = v
        self.ok = _slice_ok(v)
        self.n = _int(v["len"]) if self.ok else 0

    def to_string(self):
        if not self.ok:
            return _not_set(self.v)
        return "%s(len=%d)" % (_tag(self.v.type), self.n)

    def children(self):
        if not self.ok:
            return iter(())
        return _items(self.v["ptr"], self.n)

    def display_hint(self):
        return "array"


class ListPrinter:
    """List[T]: the first len items of buf."""

    def __init__(self, v):
        self.v = v
        try:
            buf = v["buf"]
            self.n = _int(v["len"])
            self.ok = _slice_ok(buf) and 0 <= self.n <= _int(buf["len"])
        except (gdb.MemoryError, gdb.error):
            self.ok = False

    def to_string(self):
        if not self.ok:
            return _not_set(self.v)
        return "%s(len=%d)" % (_tag(self.v.type), self.n)

    def children(self):
        if not self.ok:
            return iter(())
        return _items(self.v["buf"]["ptr"], self.n)

    def display_hint(self):
        return "array"


class DequePrinter:
    """Deque[T]: len items from head, around the end of buf."""

    def __init__(self, v):
        self.v = v
        try:
            buf = v["buf"]
            self.n = _int(v["len"])
            size = _int(buf["len"])
            head = _int(v["head"])
            self.ok = (_slice_ok(buf) and 0 <= self.n <= size and (size & (size - 1)) == 0 and
                       (size == 0 or 0 <= head < size))
        except (gdb.MemoryError, gdb.error):
            self.ok = False

    def to_string(self):
        if not self.ok:
            return _not_set(self.v)
        return "%s(len=%d)" % (_tag(self.v.type), self.n)

    def children(self):
        if not self.ok:
            return
        buf = self.v["buf"]
        size = _int(buf["len"])
        head = _int(self.v["head"])
        for i in range(min(self.n, _max_items())):
            try:
                v = buf["ptr"][(head + i) & (size - 1)]
                v.fetch_lazy()
            except (gdb.MemoryError, gdb.error):
                yield "[%d]" % i, "<unreadable>"
                return
            yield "[%d]" % i, v

    def display_hint(self):
        return "array"


def _map_ok(m):
    """slots and state have the same length, and len fits in it."""
    try:
        slots, state = m["slots"], m["state"]
        n = _int(m["len"])
        return (_slice_ok(slots) and _slice_ok(state) and _int(slots["len"]) == _int(state["len"]) and
                0 <= n <= _int(slots["len"]))
    except (gdb.MemoryError, gdb.error):
        return False


def _map_entries(m, limit):
    """The (key, value) pairs of a Map: the slots whose state has bit 0x80.
    A slot that gdb cannot read gives (None, None), and the pairs stop."""
    slots = m["slots"]
    size = _int(slots["len"])
    state = _int(m["state"]["ptr"])
    inf = gdb.selected_inferior()
    found = 0
    at = 0
    while at < size and found < limit:
        n = min(4096, size - at)
        try:
            chunk = inf.read_memory(state + at, n).tobytes()
        except (gdb.MemoryError, gdb.error):
            yield None, None
            return
        for i, b in enumerate(chunk):
            if b & 0x80:
                try:
                    e = slots["ptr"][at + i]
                    e.fetch_lazy()
                except (gdb.MemoryError, gdb.error):
                    yield None, None
                    return
                yield e["key"], e["val"]
                found += 1
                if found >= limit:
                    return
        at += n


class MapPrinter:
    """Map[K, V]: the keys and their values."""

    def __init__(self, v):
        self.v = v
        self.ok = _map_ok(v)

    def to_string(self):
        if not self.ok:
            return _not_set(self.v)
        return "%s(len=%d)" % (_tag(self.v.type), _int(self.v["len"]))

    def children(self):
        if not self.ok:
            return
        i = 0
        for k, val in _map_entries(self.v, _max_items()):
            if k is None:
                yield "[%d]" % i, "<unreadable>"
                yield "[%d]" % (i + 1), "<unreadable>"
                return
            yield "[%d]" % i, k
            yield "[%d]" % (i + 1), val
            i += 2

    def display_hint(self):
        return "map"


class SetPrinter:
    """Set[T]: the keys of its map."""

    def __init__(self, v):
        self.v = v
        try:
            n = _int(v["len"])
            self.ok = _map_ok(v["m"]) and 0 <= n <= _int(v["m"]["slots"]["len"])
        except (gdb.MemoryError, gdb.error):
            self.ok = False

    def to_string(self):
        if not self.ok:
            return _not_set(self.v)
        return "%s(len=%d)" % (_tag(self.v.type), _int(self.v["len"]))

    def children(self):
        if not self.ok:
            return
        i = 0
        for k, _ in _map_entries(self.v["m"], _max_items()):
            if k is None:
                yield "[%d]" % i, "<unreadable>"
                return
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


def duration_text(d):
    """A Duration in nanoseconds as Iris prints it: 250ms, 1.5s, 1h2m3s."""
    if d == 0:
        return "0s"
    sign = "-" if d < 0 else ""
    u = -d if d < 0 else d

    def frac(whole, part, digits, unit):
        text = "%d" % whole
        if part:
            text += "." + ("%0*d" % (digits, part)).rstrip("0")
        return text + unit

    if u < 1000:
        return sign + frac(u, 0, 1, "ns")
    if u < 1000000:
        return sign + frac(u // 1000, u % 1000, 3, "us")
    if u < 1000000000:
        return sign + frac(u // 1000000, u % 1000000, 6, "ms")
    s, part = divmod(u, 1000000000)
    text = ""
    if s >= 3600:
        text += "%dh" % (s // 3600)
    if s >= 60:
        text += "%dm" % (s % 3600 // 60)
    return sign + text + frac(s % 60, part, 9, "s")


class DurationPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        return duration_text(int(self.v))


class ChanPrinter:
    """chan[T]: the length, the room, the tasks that wait, and the items."""

    def __init__(self, v, name):
        self.v = v
        self.name = name
        self.c = None
        self.ok = True
        if int(v) != 0:
            try:
                self.c = v.cast(gdb.lookup_type("Chan").pointer()).dereference()
                n, cap = int(self.c["len"]), int(self.c["cap"])
                self.ok = 0 <= n <= cap <= _LIMIT and (cap == 0 or int(self.c["buf"]) != 0)
            except gdb.MemoryError:
                self.ok = False
            except gdb.error:
                self.c = None

    def to_string(self):
        if int(self.v) == 0:
            return "%s(null)" % self.name
        if not self.ok:
            return "%s(not set)" % self.name
        if self.c is None:
            return "%s(%#x)" % (self.name, int(self.v))
        text = "%s(len=%d, cap=%d" % (self.name, int(self.c["len"]), int(self.c["cap"]))
        if int(self.c["recvWaiters"]["head"]):
            text += ", tasks wait to receive"
        if int(self.c["sendWaiters"]["head"]):
            text += ", tasks wait to send"
        return text + ")"

    def children(self):
        if self.c is None or not self.ok:
            return
        n, cap = int(self.c["len"]), int(self.c["cap"])
        if cap <= 0:
            return
        elem = self.v.type.strip_typedefs().target()
        size = int(self.c["elemSize"])
        head = int(self.c["head"])
        buf = int(self.c["buf"])
        for i in range(min(n, _max_items())):
            at = buf + ((head + i) % cap) * size
            try:
                v = gdb.Value(at).cast(elem.pointer()).dereference()
                v.fetch_lazy()
            except (gdb.MemoryError, gdb.error):
                yield "[%d]" % i, "<unreadable>"
                return
            yield "[%d]" % i, v

    def display_hint(self):
        return "array"


class AtomicPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        return "%s(%s)" % (_tag(self.v.type), self.v["value"].format_string())


class MutexPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        state = int(self.v["state"])
        if state == 0:
            return "Mutex(unlocked)"
        if state == 2:
            return "Mutex(locked, tasks wait)"
        return "Mutex(locked)"


class FdPrinter:
    """net.Conn, net.Listener and fs.File: the descriptor, or closed, and
    the fields that tell what it did."""

    def __init__(self, v, field):
        self.v = v
        self.field = field

    def to_string(self):
        fd = int(self.v[self.field])
        name = _tag(self.v.type)
        if fd < 0:
            return "%s(closed)" % name
        extra = ""
        names = [f.name for f in self.v.type.strip_typedefs().fields()]
        if "tls" in names and int(self.v["tls"]) != 0:
            extra = ", tls"
        return "%s(fd=%d%s)" % (name, fd, extra)

    def children(self):
        for f in self.v.type.strip_typedefs().fields():
            if f.name in ("port", "received", "sent", "timeout"):
                yield f.name, self.v[f.name]


class BytePrinter:
    """u8 and i8: a number (gdb shows a byte as a character too)."""

    def __init__(self, v):
        self.v = v

    def to_string(self):
        return str(int(self.v))


class ByteArrayPrinter:
    """u8[4] and i8[4]: the numbers (gdb shows them as text)."""

    def __init__(self, v):
        self.v = v

    def children(self):
        lo, hi = self.v.type.strip_typedefs().range()
        for i in range(lo, hi + 1):
            yield "[%d]" % i, int(self.v[i])

    def display_hint(self):
        return "array"


def _is_byte(t):
    t = t.strip_typedefs()
    return t.code == gdb.TYPE_CODE_INT and t.sizeof == 1 and t.name in ("u8", "i8")


class TuplePrinter:
    """(A, B): the values in parentheses."""

    def __init__(self, v):
        self.v = v

    def to_string(self):
        parts = [self.v[f.name].format_string() for f in self.v.type.strip_typedefs().fields()]
        return "(" + ", ".join(parts) + ")"


def _is_data_enum(t):
    fields = t.fields()
    return (len(fields) == 2 and fields[0].name == "tag" and fields[1].name == "data"
            and fields[0].type.strip_typedefs().code == gdb.TYPE_CODE_ENUM
            and fields[1].type.strip_typedefs().code == gdb.TYPE_CODE_UNION)


def iris_lookup(v):
    typedef = v.type.name or ""
    if typedef.startswith("chan["):
        return ChanPrinter(v, typedef)
    t = v.type.strip_typedefs()
    if t.code == gdb.TYPE_CODE_INT and (t.name == "Duration" or typedef == "Duration"):
        return DurationPrinter(v)
    if _is_byte(t):
        return BytePrinter(v)
    if t.code == gdb.TYPE_CODE_ARRAY and _is_byte(t.target()):
        return ByteArrayPrinter(v)
    if t.code != gdb.TYPE_CODE_STRUCT:
        return None
    name = t.tag or t.name
    if not name:
        return None
    if name.startswith("Atomic["):
        return AtomicPrinter(v)
    if name == "Mutex":
        return MutexPrinter(v)
    if name in ("net.Conn", "net.Listener"):
        return FdPrinter(v, "sock")
    if name == "fs.File":
        return FdPrinter(v, "fd")
    if name.startswith("(") and name.endswith(")"):
        return TuplePrinter(v)
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


# ---------------------------------------------------------------- conditions

def text_bytes(v):
    """The bytes of a str, a String, a u8[] or a text in quotes in gdb."""
    t = v.type.strip_typedefs()
    if t.code == gdb.TYPE_CODE_PTR and t.target().strip_typedefs().code == gdb.TYPE_CODE_STRUCT:
        v = v.dereference()     # an xmethod's object comes as a pointer
        t = v.type.strip_typedefs()
    if t.code == gdb.TYPE_CODE_STRUCT:
        names = [f.name for f in t.fields()]
        if "ptr" not in names or "len" not in names:
            raise gdb.GdbError("iris: %s is not text" % (t.tag or t.name))
        n = int(v["len"])
        if _bad_len(n):
            raise gdb.GdbError("iris: text with length %d" % n)
        if n == 0:
            return b""
        return gdb.selected_inferior().read_memory(int(v["ptr"]), n).tobytes()
    if t.code == gdb.TYPE_CODE_ARRAY:
        lo, hi = t.range()
        data = bytes(int(v[i]) & 0xFF for i in range(lo, hi + 1))
        return data[:-1] if data.endswith(b"\0") else data
    if t.code == gdb.TYPE_CODE_PTR:
        return v.string(encoding="latin-1").encode("latin-1")
    raise gdb.GdbError("iris: a value of type %s is not text" % t)


class _TextFunction(gdb.Function):
    def __init__(self, name, test, doc):
        self.__doc__ = doc
        super().__init__(name)
        self.test = test

    def invoke(self, a, b):
        return 1 if self.test(text_bytes(a), text_bytes(b)) else 0


def _add_functions():
    _TextFunction("iris_eq", lambda a, b: a == b,
                  "$iris_eq(a, b): 1 when the texts a and b are the same. a and b are str, String or \"text\".")
    _TextFunction("iris_contains", lambda a, b: b in a, "$iris_contains(s, part): 1 when the text s has part in it.")
    _TextFunction("iris_starts", lambda a, b: a.startswith(b), "$iris_starts(s, start): 1 when s starts with start.")
    _TextFunction("iris_ends", lambda a, b: a.endswith(b), "$iris_ends(s, end): 1 when s ends with end.")


# ---------------------------------------------------------------- operators
#
# gdb runs xmethods only in C++ expressions: the script sets the language to
# c++ (see the end of the file). Then xs[0], m["key"], m[3], name == "iris"
# and name != "iris" work as in Iris, in print, watch and breakpoint
# conditions. A field such as xs.len works in any language.

import gdb.xmethod


def _char_ptr():
    return gdb.lookup_type("char").pointer()


def _text_types():
    """The types a text can come as: "text" in quotes, str and String."""
    out = [_char_ptr()]
    for name in ("str", "String"):
        try:
            # a C struct tag: gdb.lookup_type doesn't find it in C++ mode
            out.append(gdb.parse_and_eval("(struct %s *)0" % name).type.target())
        except gdb.error:
            pass
    return out


def _index(obj, i, n, item_at):
    i = int(i)
    if i < 0 or i >= n:
        raise gdb.GdbError("index %d out of range (len %d)" % (i, n))
    return item_at(i)


class _Worker(gdb.xmethod.XMethodWorker):
    def __init__(self, arg_type, fn, result=None):
        self.arg_type = arg_type
        self.fn = fn
        self.result = result

    def get_arg_types(self):
        return self.arg_type

    def get_result_type(self, obj, *args):
        return self.result(obj) if callable(self.result) else self.result

    def __call__(self, obj, *args):
        return self.fn(obj, *args)


def _list_item(obj, i):
    n = _int(obj["len"])
    return _index(obj, i, n, lambda k: obj["buf"]["ptr"][k])


def _slice_item(obj, i):
    n = _int(obj["len"])
    return _index(obj, i, n, lambda k: obj["ptr"][k])


def _deque_item(obj, i):
    n = _int(obj["len"])
    buf = obj["buf"]
    size = _int(buf["len"])
    head = _int(obj["head"])
    return _index(obj, i, n, lambda k: buf["ptr"][(head + k) & (size - 1)])


def _key_matches(key, want):
    kt = key.type.strip_typedefs()
    if kt.code == gdb.TYPE_CODE_STRUCT:
        return text_bytes(key) == text_bytes(want)
    return int(key) == int(want)


def _key_text(want):
    try:
        return '"%s"' % text_bytes(want).decode("utf-8", "replace")
    except gdb.GdbError:
        return str(int(want))


def _map_item(obj, key):
    if not _map_ok(obj):
        raise gdb.GdbError("iris: the map is not set")
    for k, v in _map_entries(obj, _LIMIT):
        if k is None:
            raise gdb.GdbError("iris: gdb cannot read the map")
        if _key_matches(k, key):
            return v
    raise gdb.GdbError("no key %s in the map" % _key_text(key))


class _IrisMatcher(gdb.xmethod.XMethodMatcher):
    def __init__(self):
        super().__init__("iris")

    def match(self, class_type, method_name):
        t = class_type.strip_typedefs()
        name = t.tag or t.name or ""
        i64 = gdb.lookup_type("long")
        if method_name == "operator[]":
            if name.startswith("List["):
                return _Worker(i64, _list_item, t["buf"].type.strip_typedefs()["ptr"].type.target())
            if name.startswith("Deque["):
                return _Worker(i64, _deque_item, t["buf"].type.strip_typedefs()["ptr"].type.target())
            if name.endswith("[]") or name in ("str", "String"):
                return _Worker(i64, _slice_item, t["ptr"].type.target())
            if name.startswith("Map["):
                entry = t["slots"].type.strip_typedefs()["ptr"].type.target().strip_typedefs()
                key, val = entry["key"].type, entry["val"].type
                if key.strip_typedefs().code == gdb.TYPE_CODE_STRUCT:
                    return [_Worker(a, _map_item, val) for a in _text_types()]
                return _Worker(key, _map_item, val)
            return None
        if name in ("str", "String") and method_name in ("operator==", "operator!="):
            same = method_name == "operator=="
            bool_type = gdb.lookup_type("bool")
            return [_Worker(a, lambda obj, other, same=same: (text_bytes(obj) == text_bytes(other)) == same, bool_type)
                    for a in _text_types()]
        return None


# ---------------------------------------------------------------- tasks

def _field(v, name, default=0):
    try:
        return v[name]
    except gdb.error:
        return default


def _scheds():
    """The schedulers of the threads, as gdb values of Sched *."""
    try:
        n = int(gdb.parse_and_eval("schedsLen"))
        arr = gdb.parse_and_eval("allScheds")
    except gdb.error:
        raise gdb.GdbError("iris: this program has no task list (the runtime has no debug information)")
    return [arr[i] for i in range(n)]


def _tasks():
    """Each live task: (task, sched)."""
    out = []
    for s in _scheds():
        if int(s["ended"]):
            continue
        t = s["tasks"]
        seen = 0
        while int(t) != 0 and seen < 1000000:
            out.append((t, s))
            t = t["allNext"]
            seen += 1
    out.sort(key=lambda ts: int(ts[0]["id"]))
    return out


def _thread_of(sched):
    tid = int(sched["tid"])
    for th in gdb.selected_inferior().threads():
        if th.ptid[1] == tid:
            return th
    return None


def _task_name(t):
    """The function the task runs, from the name of its spawn thunk."""
    fn = int(t["fn"])
    if fn == 0:
        return "main"
    text = gdb.execute("info symbol %d" % fn, to_string=True).strip()
    sym = text.split(" in section ")[0].split(" + ")[0]
    return sym.split(":", 1)[1] if sym.startswith("spawn.") and ":" in sym else sym


def _state(t, sched):
    if int(sched["running"]) == int(t):
        return "running"
    if int(t["ready"]):
        return "ready"
    if int(t["wakeAt"]):
        return "sleeping"
    return "waiting"


_ARCH_REGS = {
    # the registers that iris_ctx_switch saves: (register, slot from the saved sp)
    "x86_64": [("r15", 0), ("r14", 1), ("r13", 2), ("r12", 3), ("rbx", 4), ("rbp", 5)],
    "aarch64": [("x19", 0), ("x20", 1), ("x21", 2), ("x22", 3), ("x23", 4), ("x24", 5),
                ("x25", 6), ("x26", 7), ("x27", 8), ("x28", 9), ("x29", 10)],
}


def _arch():
    name = gdb.selected_inferior().architecture().name()
    if "x86-64" in name or "x86_64" in name:
        return "x86_64"
    if "aarch64" in name:
        return "aarch64"
    raise gdb.GdbError("iris: tasks on %s are not known" % name)


class _OnTaskStack:
    """Puts the registers of the selected thread on a parked task's stack, as
    iris_ctx_switch left it, and puts them back after."""

    def __init__(self, t):
        self.sp = int(t["sp"])

    def __enter__(self):
        arch = _arch()
        self.frame_level = gdb.selected_frame().level()
        gdb.newest_frame().select()
        names = [r for r, _ in _ARCH_REGS[arch]] + (["rsp", "rip"] if arch == "x86_64" else ["sp", "pc"])
        frame = gdb.newest_frame()
        self.saved = [(r, int(frame.read_register(r))) for r in names]
        sp = self.sp
        for r, slot in _ARCH_REGS[arch]:
            gdb.execute("set $%s = %d" % (r, _read_u64(sp + 8 * slot)), to_string=True)
        if arch == "x86_64":
            pc, newsp = _read_u64(sp + 48), sp + 56
            gdb.execute("set $rsp = %d" % newsp, to_string=True)
            gdb.execute("set $rip = %d" % pc, to_string=True)
        else:
            pc, newsp = _read_u64(sp + 88), sp + 176
            gdb.execute("set $sp = %d" % newsp, to_string=True)
            gdb.execute("set $pc = %d" % pc, to_string=True)
        return self

    def __exit__(self, *exc):
        gdb.newest_frame().select()
        for r, v in self.saved:
            gdb.execute("set $%s = %d" % (r, v), to_string=True)
        try:
            f = gdb.newest_frame()
            for _ in range(self.frame_level):
                f = f.older()
            f.select()
        except (gdb.error, AttributeError):
            pass
        return False


def _iris_frame():
    """(file, line, function, waits, parked) of the newest frame in an .iris
    file. waits is the runtime call the task waits in (the newest iris_
    function above it), and parked tells if the scheduler is above it."""
    f = gdb.newest_frame()
    waits = None
    parked = False
    while f is not None:
        sal = f.find_sal()
        name = f.name() or ""
        if sal.symtab and sal.symtab.filename.endswith(".iris"):
            return (sal.symtab.fullname(), sal.line, name, waits if parked else None, parked)
        if name == "schedule":
            parked = True
        if name.startswith("iris_") and waits is None:
            waits = name
        try:
            f = f.older()
        except gdb.error:
            break
    return (None, 0, None, waits if parked else None, parked)


def _where(t, sched):
    """_iris_frame for the task, on its own stack."""
    th = _thread_of(sched)
    if th is None:
        return None
    old = gdb.selected_thread()
    try:
        th.switch()
        if _state(t, sched) == "running":
            return _iris_frame()
        with _OnTaskStack(t):
            return _iris_frame()
    finally:
        old.switch()


def task_list():
    """The tasks as dicts: id, thread (gdb's number), lwp, state, function,
    file, line, frame (the function of the newest Iris frame), waits, current."""
    sel = gdb.selected_thread()
    out = []
    for t, s in _tasks():
        th = _thread_of(s)
        state = _state(t, s)
        try:
            where = _where(t, s)
        except gdb.error:
            where = None
        file, line, frame, waits, parked = where if where else (None, 0, None, None, False)
        if state == "running" and parked:
            # the thread's task, which waits in the scheduler for the system
            state = "sleeping" if int(t["wakeAt"]) else "waiting"
        if int(t["sp"]) and _read_u64(int(t["sp"]) + 48 if _arch() == "x86_64" else int(t["sp"]) + 88) == \
                int(gdb.parse_and_eval("(long)&taskEntry")):
            state = "new" if state == "ready" else state     # spawned, not started yet
        out.append({
            "id": int(t["id"]),
            "thread": th.num if th else 0,
            "lwp": int(s["tid"]),
            "state": state,
            "function": _task_name(t),
            "file": file,
            "line": line,
            "frame": frame,
            "waits": waits,
            "current": int(s["running"]) == int(t) and th is not None and th == sel,
        })
    return out


class IrisCommand(gdb.Command):
    """Commands for Iris programs: iris tasks, iris task N [CMD]."""

    def __init__(self):
        super().__init__("iris", gdb.COMMAND_DATA, gdb.COMPLETE_NONE, True)


class IrisTasksCommand(gdb.Command):
    """List the Iris tasks of the program: iris tasks [--json].

Each line has the task's number, gdb's thread, the state (running, ready,
new, sleeping or waiting), the function it runs, where it is in Iris code,
and the runtime call it waits in. "*" marks the task of the selected thread.
A task of a thread that waits for the system shows as sleeping or waiting."""

    def __init__(self):
        super().__init__("iris tasks", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        tasks = task_list()
        if arg.strip() == "--json":
            gdb.write(json.dumps(tasks) + "\n")
            return
        gdb.write("  %-5s %-7s %-9s %-20s %-28s %s\n" % ("Id", "Thread", "State", "Function", "Where", "Waits in"))
        for t in tasks:
            where = "%s:%d" % (os.path.basename(t["file"]), t["line"]) if t["file"] else "-"
            if t["frame"] and t["frame"] != t["function"]:
                where += " in " + t["frame"]
            gdb.write("%s %-5d %-7d %-9s %-20s %-28s %s\n" % (
                "*" if t["current"] else " ", t["id"], t["thread"], t["state"], t["function"], where,
                t["waits"] or ""))


class IrisTaskCommand(gdb.Command):
    """Run a command on the stack of an Iris task: iris task N [CMD].

CMD is backtrace when none is given. Several commands go apart with ";",
such as: iris task 3 frame 4; info locals. For a parked task the registers
of a thread are put on the task's stack while CMD runs, and back after."""

    def __init__(self):
        super().__init__("iris task", gdb.COMMAND_STACK)

    def invoke(self, arg, from_tty):
        parts = arg.strip().split(None, 1)
        if not parts or not parts[0].isdigit():
            raise gdb.GdbError("usage: iris task N [CMD]")
        n = int(parts[0])
        cmds = [c.strip() for c in (parts[1] if len(parts) > 1 else "backtrace").split(";") if c.strip()]
        for t, s in _tasks():
            if int(t["id"]) != n:
                continue
            th = _thread_of(s)
            if th is None:
                raise gdb.GdbError("iris: task %d has no thread that gdb knows" % n)
            old = gdb.selected_thread()
            th.switch()
            try:
                if _state(t, s) == "running":
                    for c in cmds:
                        gdb.execute(c, from_tty)
                else:
                    with _OnTaskStack(t):
                        for c in cmds:
                            gdb.execute(c, from_tty)
            finally:
                old.switch()
            return
        raise gdb.GdbError("iris: no task %d" % n)


# "finish" and the return value. On x86_64, LLVM returns a struct in up to 3
# integer registers (rax, rdx, rcx) and 4 float registers (xmm0, xmm1, st0,
# st1), one leaf of the struct in each. C puts 2 eightbytes in registers and
# a larger struct in memory, and gdb reads the value by the C rules. So gdb
# shows the wrong value for a List, a String, a tuple or a struct of i32.
# This "finish" reads the registers as LLVM sets them.

_INT_RET = ("rax", "rdx", "rcx")
_FLOAT_RET = ("xmm0", "xmm1", "st0", "st1")


def _leaves(t, off, out):
    """Add the scalar leaves of t that LLVM returns, as (offset, size, is_float).
    Return False for a layout that is not certain, such as a bit field."""
    t = t.strip_typedefs()
    c = t.code
    if c == gdb.TYPE_CODE_STRUCT:
        end = 0
        for f in t.fields():
            if not hasattr(f, "bitpos") or f.is_base_class:
                continue
            if f.bitsize or f.bitpos % 8:
                return False
            at = f.bitpos // 8
            ft = f.type.strip_typedefs()
            # check.c puts an [N x i8] slot into a gap that LLVM would not leave
            if at - end >= max(1, min(ft.alignof, 8)):
                return False
            if not _leaves(ft, off + at, out):
                return False
            end = at + ft.sizeof
        return True
    if c == gdb.TYPE_CODE_ARRAY:
        el = t.target()
        if el.sizeof == 0:
            return True
        for i in range(t.sizeof // el.sizeof):
            if not _leaves(el, off + i * el.sizeof, out):
                return False
        return True
    if c == gdb.TYPE_CODE_UNION:
        # the payload of an enum with data: an [N x i64] array in the IR
        for i in range((t.sizeof + 7) // 8):
            out.append((off + 8 * i, 8, False))
        return True
    if c == gdb.TYPE_CODE_FLT:
        if t.sizeof not in (4, 8):
            return False
        out.append((off, t.sizeof, True))
        return True
    if c in (gdb.TYPE_CODE_INT, gdb.TYPE_CODE_PTR, gdb.TYPE_CODE_BOOL, gdb.TYPE_CODE_ENUM,
             gdb.TYPE_CODE_CHAR):
        if t.sizeof > 8:
            return False
        out.append((off, t.sizeof, False))
        return True
    return False


def _float_bytes(frame, reg, size):
    v = frame.read_register(reg)
    if reg.startswith("st"):
        return struct.pack("<f" if size == 4 else "<d", float(v))
    return int(v["uint128"]).to_bytes(16, "little")[:size]


def _llvm_leaves(t):
    """Give the leaves of t when LLVM returns t in registers on x86_64 and C
    does not. Give None for a scalar, for a struct that LLVM returns in memory
    (as C does, with the address in rax) and for other machines."""
    if t is None or _arch_name() != "x86_64":
        return None
    t = t.strip_typedefs()
    if t.code not in (gdb.TYPE_CODE_STRUCT, gdb.TYPE_CODE_ARRAY, gdb.TYPE_CODE_UNION):
        return None
    leaves = []
    if not _leaves(t, 0, leaves):
        return None
    if sum(not l[2] for l in leaves) > len(_INT_RET) or sum(l[2] for l in leaves) > len(_FLOAT_RET):
        return None
    return leaves


def _arch_name():
    try:
        return _arch()
    except gdb.GdbError:
        return None


def _llvm_return(frame, t, leaves):
    """Read a value of type t from the registers where LLVM returns it."""
    t = t.strip_typedefs()
    buf = bytearray(t.sizeof)
    ints = [l for l in leaves if not l[2]]
    floats = [l for l in leaves if l[2]]
    for (off, size, _), reg in zip(ints, _INT_RET):
        n = int(frame.read_register(reg)) & ((1 << 64) - 1)
        part = n.to_bytes(8, "little")[:min(size, t.sizeof - off)]
        buf[off:off + len(part)] = part
    for (off, size, _), reg in zip(floats, _FLOAT_RET):
        buf[off:off + size] = _float_bytes(frame, reg, size)
    return gdb.Value(bytes(buf), t)


class _RegisterReturn(gdb.Breakpoint):
    """Stops where the frame returns to its caller, in the same thread, and
    reads the value that LLVM put in the registers."""

    def __init__(self, frame, rtype, leaves):
        self.caller = frame.older()
        super().__init__("*0x%x" % self.caller.pc(), internal=True)
        self.thread = gdb.selected_thread().global_num
        self.rtype = rtype
        self.leaves = leaves
        self.value = None
        self.hit = False

    def stop(self):
        if gdb.newest_frame() != self.caller:
            return False            # the return of a deeper call
        self.hit = True
        try:
            self.value = _llvm_return(gdb.newest_frame(), self.rtype, self.leaves)
        except gdb.error:
            self.value = None
        return True


class _GdbReturn(gdb.FinishBreakpoint):
    """gdb's own finish breakpoint, for a value that gdb reads right."""

    def __init__(self, frame):
        super().__init__(frame, internal=True)
        self.value = None
        self.hit = False

    def stop(self):
        self.hit = True
        if self.return_value is not None:
            self.value = self.return_value
            self.value.fetch_lazy()
        return True

    def out_of_scope(self):
        pass


def _return_type(frame):
    fn = frame.function()
    if fn is None:
        return None
    t = fn.type.strip_typedefs()
    if t.code != gdb.TYPE_CODE_FUNC or t.target() is None:
        return None
    if t.target().strip_typedefs().code == gdb.TYPE_CODE_VOID:
        return None
    return t.target()


class IrisFinishCommand(gdb.Command):
    """Run until the selected frame returns, and print the value that it returns.

This is gdb's finish, with the value read from the registers where the Iris
compiler puts it: a List, a String, a tuple or a struct shows right. gdb's
own finish stays as it is, for MI (-exec-finish) and for other values."""

    def __init__(self):
        super().__init__("iris finish", gdb.COMMAND_RUNNING)

    def invoke(self, arg, from_tty):
        if arg.strip():
            raise gdb.GdbError("usage: iris finish")
        frame = gdb.selected_frame()
        if frame.older() is None:
            raise gdb.GdbError('"iris finish" not meaningful in the outermost frame.')
        where = gdb.execute("frame", to_string=True).splitlines()[0]
        gdb.write("Run till exit from %s\n" % where)
        rtype = _return_type(frame)
        leaves = _llvm_leaves(rtype)
        bp = _RegisterReturn(frame, rtype, leaves) if leaves else _GdbReturn(frame)
        bp.silent = True            # gdb's finish shows no breakpoint number
        try:
            gdb.execute("continue")
        finally:
            hit, value = bp.hit, bp.value
            if bp.is_valid():
                bp.delete()
        if not hit:
            return                  # a breakpoint or a signal stopped it, and gdb said so
        gdb.write(re.sub(r"^#0\s+", "", gdb.execute("frame", to_string=True)))
        if value is not None:
            n = gdb.add_history(value)
            gdb.write("Value returned is $%d = %s\n" % (n, value.format_string()))


def _drop_c_main(bp):
    """"break main" finds the Iris main and the C main that calls it. The C
    main (iris.start in the debug info) is compiler-made: its location goes off."""
    try:
        locs = bp.locations
    except (AttributeError, RuntimeError):
        return
    if len(locs) < 2:
        return
    for l in locs:
        if l.enabled and (l.function == "iris.start" or (l.source is None and l.function == "main")):
            l.enabled = False


def _function_type(name):
    """The return type of the function NAME, or None."""
    for look in (lambda: gdb.lookup_symbol(name)[0], lambda: gdb.lookup_static_symbol(name),
                 lambda: gdb.lookup_global_symbol(name)):
        try:
            sym = look()
        except gdb.error:
            sym = None
        if sym is not None and sym.type is not None and sym.type.strip_typedefs().code == gdb.TYPE_CODE_FUNC:
            r = sym.type.strip_typedefs().target()
            return None if r is None or r.strip_typedefs().code == gdb.TYPE_CODE_VOID else r
    return None


if hasattr(gdb, "MICommand"):
    class IrisReturnValueMI(gdb.MICommand):
        """-iris-return-value FUNCTION: the value that FUNCTION returned, right
        after -exec-finish stops in its caller. The result has gdb-result-var
        and return-value as -exec-finish does, or registers="no" when gdb's
        own return-value of -exec-finish is right for this type."""

        def __init__(self):
            super().__init__("-iris-return-value")

        def invoke(self, argv):
            if len(argv) != 1:
                raise gdb.GdbError("usage: -iris-return-value FUNCTION")
            rtype = _function_type(argv[0])
            leaves = _llvm_leaves(rtype)
            if not leaves:
                return {"registers": "no"}
            value = _llvm_return(gdb.newest_frame(), rtype, leaves)
            n = gdb.add_history(value)
            return {"registers": "yes", "gdb-result-var": "$%d" % n, "return-value": value.format_string()}


# Each program can load the script again: it adds the printers once.
if not any(getattr(p, "__name__", "") == "iris_lookup" for p in gdb.pretty_printers):
    gdb.pretty_printers.append(iris_lookup)
    IrisCommand()
    IrisTasksCommand()
    IrisTaskCommand()
    IrisFinishCommand()
    if hasattr(gdb, "MICommand"):
        IrisReturnValueMI()
    gdb.events.breakpoint_created.connect(_drop_c_main)
    gdb.events.breakpoint_modified.connect(_drop_c_main)
    _add_functions()
    gdb.xmethod.register_xmethod_matcher(None, _IrisMatcher())
    # the operators above need C++ expressions; Iris frames are C, so gdb
    # would warn at each stop that the language is not the frame's
    gdb.execute("set language c++")
    gdb.execute("set warn-language-frame-mismatch off")
    # step goes into Iris code only: over the runtime (C) and over the next
    # methods of the std types that for loops call
    gdb.execute("skip -gfile */runtime/rt.c", to_string=True)
    gdb.execute("skip -gfile */std/*/*.iris -rfunction \\.next$", to_string=True)
