"""gdb script: run ./main on one input file and record every call into main.c.

Not meant to be run by hand; tools/trace.py drives it. Inputs come from the
environment (TRACE_SRC, TRACE_DB, TRACE_IN, TRACE_STDOUT, TRACE_OUT).

Every function defined in main.c gets a breakpoint. On entry we record the
arguments (dereferencing the REPL's own structs, and mapping raw pointers to
"page N + byte K" inside the pager's cache); a FinishBreakpoint records the
return value and the arguments' state afterwards. Snapshots of the page cache
(memory) and of the .db file (disk) are taken before every prompt, after every
pager_flush and after the process exits.
"""

import json
import os
import re
import struct

import gdb

SRC = os.environ["TRACE_SRC"]
DB = os.environ["TRACE_DB"]
IN = os.environ["TRACE_IN"]
STDOUT = os.environ["TRACE_STDOUT"]
OUT = os.environ["TRACE_OUT"]

FUNC_RE = re.compile(r"^[A-Za-z_][\w \t*]*?\b(\w+)\s*\([^;{)]*\)\s*\{", re.M)
FUNCS = list(dict.fromkeys(m.group(1) for m in FUNC_RE.finditer(open(SRC).read())))

events = []
state = {"pager": None, "consts": None, "ptr_count": None}


def const(name):
    return int(gdb.parse_and_eval(name))


def consts():
    if state["consts"] is None:
        names = ["PAGE_SIZE", "LEAF_NODE_NUM_CELLS_OFFSET", "LEAF_NODE_HEADER_SIZE",
                 "LEAF_NODE_CELL_SIZE", "LEAF_NODE_KEY_SIZE", "LEAF_NODE_MAX_CELLS",
                 "ID_OFFSET", "USERNAME_OFFSET", "EMAIL_OFFSET", "USERNAME_SIZE", "EMAIL_SIZE"]
        state["consts"] = {n: const(n) for n in names}
        # Internal-node layout: not in main.c yet (it arrives with node splitting in
        # the tutorial). Picked up automatically once these constants exist.
        try:
            state["consts"].update({n: const(n) for n in INTERNAL_CONSTS})
            state["consts"]["NODE_INTERNAL"] = const("NODE_INTERNAL")
        except gdb.error:
            pass
    return state["consts"]


INTERNAL_CONSTS = ["INTERNAL_NODE_NUM_KEYS_OFFSET", "INTERNAL_NODE_RIGHT_CHILD_OFFSET",
                   "INTERNAL_NODE_HEADER_SIZE", "INTERNAL_NODE_CELL_SIZE",
                   "INTERNAL_NODE_CHILD_SIZE"]


def page_ptrs():
    """Addresses held in pager->pages[], read in one memory access."""
    pager = state["pager"]
    if pager is None:
        return []
    arr = pager.dereference()["pages"]
    n = arr.type.range()[1] + 1
    raw = bytes(gdb.selected_inferior().read_memory(int(arr.address), 8 * n))
    return list(struct.unpack(f"<{n}Q", raw))


def locate(addr):
    size = consts()["PAGE_SIZE"]
    for i, base in enumerate(page_ptrs()):
        if base and base <= addr < base + size:
            return i, addr - base
    return None


def cstr(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def parse_node(data):
    """Decode one page: its common header, then its body as a leaf or internal node."""
    c = consts()
    header = {"node_type": data[0], "is_root": data[1],
              "parent": struct.unpack_from("<I", data, 2)[0]}
    if "INTERNAL_NODE_HEADER_SIZE" in c and data[0] == c["NODE_INTERNAL"]:
        return dict(header, kind="internal", **parse_internal(data, c))
    return dict(header, kind="leaf", **parse_leaf(data, c))


def parse_internal(data, c):
    """An internal node: (child page, key) cells plus a right-most child pointer."""
    n = struct.unpack_from("<I", data, c["INTERNAL_NODE_NUM_KEYS_OFFSET"])[0]
    right = struct.unpack_from("<I", data, c["INTERNAL_NODE_RIGHT_CHILD_OFFSET"])[0]
    max_keys = (len(data) - c["INTERNAL_NODE_HEADER_SIZE"]) // c["INTERNAL_NODE_CELL_SIZE"]
    children, keys = [], []
    for i in range(min(n, max_keys)):
        start = c["INTERNAL_NODE_HEADER_SIZE"] + i * c["INTERNAL_NODE_CELL_SIZE"]
        children.append(struct.unpack_from("<I", data, start)[0])
        keys.append(struct.unpack_from("<I", data, start + c["INTERNAL_NODE_CHILD_SIZE"])[0])
    return {"num_keys": n, "keys": keys, "children": children + [right], "cells": []}


def parse_leaf(data, c):
    """A leaf node: its cell count and every cell's key + row."""
    n = struct.unpack_from("<I", data, c["LEAF_NODE_NUM_CELLS_OFFSET"])[0]
    cells = []
    for i in range(min(n, c["LEAF_NODE_MAX_CELLS"])):
        start = c["LEAF_NODE_HEADER_SIZE"] + i * c["LEAF_NODE_CELL_SIZE"]
        row = start + c["LEAF_NODE_KEY_SIZE"]
        key = struct.unpack_from("<I", data, start)[0]
        rid = struct.unpack_from("<I", data, row + c["ID_OFFSET"])[0]
        u = row + c["USERNAME_OFFSET"]
        e = row + c["EMAIL_OFFSET"]
        cells.append({"offset": start, "key": key, "id": rid,
                      "username": cstr(data[u:u + c["USERNAME_SIZE"]]),
                      "email": cstr(data[e:e + c["EMAIL_SIZE"]])})
    return {"num_cells": n, "cells": cells, "max_cells": c["LEAF_NODE_MAX_CELLS"]}


def read_disk():
    size = consts()["PAGE_SIZE"]
    try:
        data = open(DB, "rb").read()
    except FileNotFoundError:
        return {"exists": False, "size": 0, "pages": []}
    pages = [dict(page=i // size, **parse_node(data[i:i + size]))
             for i in range(0, len(data) - size + 1, size)]
    return {"exists": True, "size": len(data), "pages": pages}


def snapshot(label, memory=True):
    mem = None
    if memory and state["pager"] is not None:
        inf = gdb.selected_inferior()
        size = consts()["PAGE_SIZE"]
        mem = {"num_pages": int(state["pager"].dereference()["num_pages"]), "pages": []}
        for i, base in enumerate(page_ptrs()):
            if base:
                mem["pages"].append(dict(page=i, **parse_node(bytes(inf.read_memory(base, size)))))
    events.append({"type": "snap", "label": label, "mem": mem, "disk": read_disk()})


def string_at(v, limit=120):
    try:
        return v.string()[:limit] if int(v) else None
    except gdb.error:
        return "<unreadable>"


def describe(v):
    """(text, data) for one value; data is a plain dict for the REPL's own structs."""
    try:
        st = v.type.strip_typedefs()
        if st.code != gdb.TYPE_CODE_PTR:
            if st.code == gdb.TYPE_CODE_BOOL:
                return ("true" if bool(v) else "false"), None
            return str(v), None
        addr = int(v)
        if addr == 0:
            return "NULL", None
        tn = str(st.target().unqualified())
        if tn == "char":
            s = string_at(v)
            return json.dumps(s), {"string": s}
        if tn == "void":
            loc = locate(addr)
            if loc:
                return f"&[page {loc[0]} + byte {loc[1]}]", {"page": loc[0], "offset": loc[1]}
            return hex(addr), None
        d = v.dereference()
        if tn == "InputBuffer":
            data = {"buffer": string_at(d["buffer"]), "input_length": int(d["input_length"])}
            return f'InputBuffer{{buffer={json.dumps(data["buffer"])}}}', data
        if tn == "Row":
            data = row_data(d)
            return f"Row{{id={data['id']}, username={json.dumps(data['username'])}, " \
                   f"email={json.dumps(data['email'])}}}", data
        if tn == "Statement":
            data = {"type": str(d["type"]), "row": row_data(d["row_to_insert"])}
            if data["type"] == "STATEMENT_INSERT":
                r = data["row"]
                return f"Statement{{INSERT, row=({r['id']}, {r['username']}, {r['email']})}}", data
            return f"Statement{{{data['type'].replace('STATEMENT_', '')}}}", data
        if tn == "Cursor":
            data = {"page_num": int(d["page_num"]), "cell_num": int(d["cell_num"]),
                    "end_of_table": bool(d["end_of_table"])}
            return f"Cursor{{page={data['page_num']}, cell={data['cell_num']}, " \
                   f"end_of_table={'true' if data['end_of_table'] else 'false'}}}", data
        if tn == "Table":
            p = d["pager"]
            n = int(p.dereference()["num_pages"]) if int(p) else 0
            return f"Table{{root_page={int(d['root_page_num'])}, pager.num_pages={n}}}", None
        if tn == "Pager":
            return f"Pager{{fd={int(d['file_descriptor'])}, file_length={int(d['file_length'])}, " \
                   f"num_pages={int(d['num_pages'])}}}", None
        loc = locate(addr)
        where = f"page {loc[0]} + byte {loc[1]}" if loc else hex(addr)
        if tn in ("uint32_t", "unsigned int"):
            return f"&[{where}] = {int(d)}", {"page": loc and loc[0], "offset": loc and loc[1]}
        return f"&[{where}]", {"page": loc and loc[0], "offset": loc and loc[1]}
    except gdb.error as e:
        return f"<{e}>", None


def row_data(d):
    return {"id": int(d["id"]), "username": d["username"].string(), "email": d["email"].string()}


def depth():
    f, n = gdb.newest_frame(), 0
    while f is not None and f.name() != "main":
        n += 1
        f = f.older()
    return n


def frame_args(frame):
    block = frame.block()
    while block.function is None:
        block = block.superblock
    return [(s.name, s.value(frame)) for s in block if s.is_argument]


def is_struct_ptr(v):
    st = v.type.strip_typedefs()
    return st.code == gdb.TYPE_CODE_PTR and st.target().strip_typedefs().code == gdb.TYPE_CODE_STRUCT


class Finish(gdb.FinishBreakpoint):
    def __init__(self, frame, call, argvals):
        super().__init__(frame, internal=True)
        self.call, self.argvals = call, argvals

    def stop(self):
        call = self.call
        call["returned"] = True
        if self.return_value is not None:
            call["ret"], call["ret_data"] = describe(self.return_value)
        # Skip the after-state of functions that free their arguments.
        if call["fn"] not in ("db_close", "close_input_buffer"):
            call["after"] = [(n, *describe(v)) for n, v in self.argvals if is_struct_ptr(v)]
        on_return(call, self.return_value)
        return False

    def out_of_scope(self):
        self.call["noreturn"] = True


class Entry(gdb.Breakpoint):
    def __init__(self, fn):
        super().__init__(fn, internal=True)
        self.fn = fn

    def stop(self):
        frame = gdb.selected_frame()
        argvals = frame_args(frame)
        if self.fn == "print_prompt":
            snapshot("before prompt")
        call = {"type": "call", "fn": self.fn, "depth": depth(),
                "args": [(n, *describe(v)) for n, v in argvals], "notes": []}
        on_entry(call, dict(argvals))
        events.append(call)
        if self.fn != "main":
            Finish(frame, call, argvals)
        return False


def on_entry(call, args):
    fn = call["fn"]
    if fn == "get_page" and state["pager"] is not None:
        num = int(args["page_num"])
        pager = args["pager"].dereference()
        if int(pager["pages"][num]) == 0:
            size = consts()["PAGE_SIZE"]
            on_disk = int(pager["file_length"]) // size
            call["notes"].append(f"cache MISS: malloc a 4 KB page, then read it from the file"
                                 if num < on_disk else
                                 "cache MISS: malloc a 4 KB page; the file has no such page yet, "
                                 "so it starts blank")
            call["miss"] = True
        else:
            call["notes"].append("cache hit: page already in memory")
    if fn == "pager_flush":
        size = consts()["PAGE_SIZE"]
        num = int(args["page_num"])
        call["notes"].append(f"write page {num} ({size} B) to file offset {num * size}")


def on_return(call, rv):
    fn = call["fn"]
    if fn == "pager_open" and rv is not None:
        state["pager"] = rv
    if fn == "serialize_row":
        dest = next((d for n, _, d in call["args"] if n == "destination"), None)
        if dest and dest.get("page") is not None:
            call["notes"].append(f"row bytes now live in page {dest['page']} at byte {dest['offset']} "
                                 "(memory only until .exit)")
    if fn == "leaf_node_insert":
        cur = next((d for n, _, d in call["args"] if n == "cursor"), None)
        if cur:
            base = page_ptrs()[cur["page_num"]]
            data = bytes(gdb.selected_inferior().read_memory(base, consts()["PAGE_SIZE"]))
            call["notes"].append(f"page {cur['page_num']} now has {parse_node(data)['num_cells']} cell(s)")
    if fn == "pager_flush":
        snapshot("after pager_flush")
    if fn == "db_close":
        state["pager"] = None


def on_exit(event):
    # Calls still open when the process ended were cut short by exit().
    for e in events:
        if e["type"] == "call" and e["fn"] != "main" and not e.get("returned"):
            e["noreturn"] = True
    snapshot("after exit", memory=False)
    out = open(STDOUT, errors="replace").read() if os.path.exists(STDOUT) else ""
    json.dump({"events": events, "stdout": out, "funcs": FUNCS,
               "exit_code": getattr(event, "exit_code", None)}, open(OUT, "w"))


gdb.execute("set pagination off")
gdb.execute("set confirm off")
for fn in FUNCS:
    Entry(fn)
gdb.events.exited.connect(on_exit)
gdb.execute(f"run {DB} < {IN} > {STDOUT}")
