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

import json
import os
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


# Each program can load the script again: it adds the printers once.
if not any(getattr(p, "__name__", "") == "iris_lookup" for p in gdb.pretty_printers):
    gdb.pretty_printers.append(iris_lookup)
    IrisCommand()
    IrisTasksCommand()
    IrisTaskCommand()
