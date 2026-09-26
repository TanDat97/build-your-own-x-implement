#!/usr/bin/env python3
"""Trace real runs of ./main under gdb and render them as output/trace.html.

Usage: python3 tools/trace.py [commands.txt]

commands.txt holds REPL input, one command per line. A line containing only
`---` ends one session and starts another: ./main is restarted on the same
output/trace.db, which shows the data coming back from the file. With no file, a
default two-session script is used.

For every command the page shows the data flowing Input -> Statement -> Cursor
-> page cache (memory) -> trace.db (disk), and the full call tree with each
call's arguments, return value and the description from the comment above that
function in main.c.
"""

import html
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = "output"
DB = os.path.join(OUT_DIR, "trace.db")
OUT = os.path.join(OUT_DIR, "trace.html")

DEFAULT_SESSIONS = [
    ["insert 1 alice alice@example.com", "insert 2 bob bob@example.com", "select", ".exit"],
    ["insert 3 carol carol@example.com", "insert 4 dave dave@example.com",
     "insert 5 eve eve@example.com", "select", ".exit"],
]

# Layer of each function, in the order data flows through them. Functions not
# listed (added to main.c later) still appear, under "Other".
LAYERS = [
    ("Input", ["new_input_buffer", "print_prompt", "read_input", "close_input_buffer"]),
    ("Front end", ["do_meta_command", "prepare_statement", "prepare_insert",
                   "print_constants", "print_leaf_node"]),
    ("Virtual machine", ["execute_statement", "execute_insert", "execute_select", "print_row"]),
    ("Cursor", ["table_start", "table_end", "cursor_value", "cursor_advance"]),
    ("B-tree node / row", ["leaf_node_insert", "leaf_node_num_cells", "leaf_node_cell",
                           "leaf_node_key", "leaf_node_value", "initialize_leaf_node",
                           "serialize_row", "deserialize_row"]),
    ("Pager", ["pager_open", "get_page", "pager_flush"]),
    ("Database", ["db_open", "db_close", "main"]),
]
LAYER_OF = {fn: i for i, (_, fns) in enumerate(LAYERS) for fn in fns}
# Pure pointer arithmetic, hidden by default to keep the call tree readable.
HELPERS = {"leaf_node_num_cells", "leaf_node_cell", "leaf_node_key", "leaf_node_value"}

FUNC_RE = re.compile(r"^([A-Za-z_][\w \t*]*?\b(\w+)\s*\([^;{)]*\))\s*\{", re.M)


def function_docs(src):
    """name -> (signature, comment above it) for every function defined in main.c."""
    docs = {}
    lines = src.splitlines()
    for m in FUNC_RE.finditer(src):
        line_no = src.count("\n", 0, m.start())
        comment = []
        i = line_no - 1
        while i >= 0 and lines[i].lstrip().startswith("//"):
            comment.insert(0, lines[i].lstrip()[2:].strip())
            i -= 1
        docs.setdefault(m.group(2), (" ".join(m.group(1).split()), " ".join(comment),
                                     line_no + 1))
    return docs


def read_sessions(path):
    if not path:
        return DEFAULT_SESSIONS
    sessions, cur = [], []
    for line in open(path).read().splitlines():
        if line.strip() == "---":
            sessions.append(cur)
            cur = []
        elif line.strip():
            cur.append(line)
    sessions.append(cur)
    return [s for s in sessions if s]


def run_session(commands, tmp, n):
    inp = os.path.join(tmp, f"in{n}.txt")
    with open(inp, "w") as f:
        f.write("\n".join(commands) + "\n")
    out_json = os.path.join(tmp, f"trace{n}.json")
    env = dict(os.environ, TRACE_SRC="main.c", TRACE_DB=DB, TRACE_IN=inp,
               TRACE_STDOUT=os.path.join(tmp, f"stdout{n}.txt"), TRACE_OUT=out_json)
    subprocess.run(["gdb", "-q", "-batch", "-nx", "-iex", "set debuginfod enabled off",
                    "-x", os.path.join(HERE, "trace_gdb.py"), "./main"],
                   env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if not os.path.exists(out_json):
        sys.exit(f"gdb produced no trace for session {n}")
    return json.load(open(out_json))


# ---------------------------------------------------------------- analysis

def split_steps(trace):
    """Cut a session's events into steps, one per prompt (plus the startup before it)."""
    steps = [{"events": []}]
    for e in trace["events"]:
        if e["type"] == "call" and e["fn"] == "print_prompt":
            steps.append({"events": []})
        steps[-1]["events"].append(e)
    chunks = trace["stdout"].split("db > ")
    for k, step in enumerate(steps):
        calls = [e for e in step["events"] if e["type"] == "call"]
        snaps = [e for e in step["events"] if e["type"] == "snap"]
        step["calls"] = calls
        step["snap"] = snaps[-1] if snaps else None
        step["mem"] = next((s["mem"] for s in reversed(snaps) if s["mem"]), None)
        step["flushes"] = [s for s in snaps if s["label"] == "after pager_flush"]
        step["output"] = chunks[k] if 0 < k < len(chunks) else ""
        step["command"] = None
        for c in calls:
            if c["fn"] == "read_input":
                buf = after(c, "input_buffer")
                if buf and c.get("returned"):
                    step["command"] = buf.get("buffer")
    return steps


def after(call, name):
    """The data of argument `name` once the call returned (falling back to on entry)."""
    for n, _, data in call.get("after", []):
        if n == name:
            return data
    for n, _, data in call["args"]:
        if n == name:
            return data
    return None


def cells_by_page(pages):
    return {p["page"]: p["cells"] for p in (pages or [])}


# ---------------------------------------------------------------- rendering

def esc(s):
    return html.escape(str(s))


def clip(s, n=110):
    s = str(s)
    return s if len(s) <= n else s[:n - 1] + "…"


def layer_class(fn):
    return f"L{LAYER_OF.get(fn, len(LAYERS))}"


def render_cells(cells, highlight=(), empty="no cells"):
    if not cells:
        return f'<p class="muted small">{empty}</p>'
    rows = []
    for c in cells:
        cls = ' class="new"' if (c["offset"], c["key"]) in highlight else ""
        rows.append(f"<tr{cls}><td>{c['offset']}</td><td>{c['key']}</td>"
                    f"<td>({c['id']}, {esc(c['username'])}, {esc(c['email'])})</td></tr>")
    return ('<table class="cells"><thead><tr><th>byte</th><th>key</th><th>row</th></tr></thead>'
            f"<tbody>{''.join(rows)}</tbody></table>")


def render_store(title, pages, other_pages, sub, prev_pages=None):
    """One of the two storage boxes (memory or disk), highlighting cells that differ."""
    if pages is None:
        return (f'<div class="box store"><h4>{title}</h4><p class="sub">{sub}</p>'
                '<p class="muted small">freed (process exited)</p></div>')
    other = cells_by_page(other_pages)
    prev = cells_by_page(prev_pages) if prev_pages is not None else None
    body = []
    for p in pages:
        # Compare with this page's previous state; a page that was not cached before
        # was just read from the file, so compare it with the file's copy instead.
        if prev is not None and (p["page"] in prev or not other):
            before = prev.get(p["page"], [])
        else:
            before = other.get(p["page"], [])
        seen = {(c["offset"], c["key"], c["id"]) for c in before}
        diff = {(c["offset"], c["key"]) for c in p["cells"]
                if (c["offset"], c["key"], c["id"]) not in seen}
        body.append(f'<div class="pg"><b>page {p["page"]}</b> '
                    f'<span class="muted">num_cells = {p.get("num_cells", p.get("num_keys"))}</span>'
                    f'{render_cells(p["cells"], diff)}</div>')
    if not pages:
        body.append('<p class="muted small">no pages</p>')
    return f'<div class="box store"><h4>{title}</h4><p class="sub">{sub}</p>{"".join(body)}</div>'


def sync_badge(mem, disk):
    if mem is None:
        return '<span class="badge ok">✓ process exited; the file is all that remains</span>'
    m = {(p["page"], c["offset"], c["key"], c["id"]) for p in mem["pages"] for c in p["cells"]}
    d = {(p["page"], c["offset"], c["key"], c["id"]) for p in disk["pages"] for c in p["cells"]}
    unsaved = len(m - d)
    if unsaved:
        return (f'<span class="badge warn">⚠ {unsaved} row(s) exist only in memory. '
                'Nothing is written until .exit calls db_close → pager_flush</span>')
    return '<span class="badge ok">✓ memory and file hold the same rows</span>'


def flow_boxes(step):
    calls = step["calls"]
    fns = [c["fn"] for c in calls]
    boxes = []

    cmd = step["command"]
    boxes.append(("Input", "read_input()",
                  f"<code>{esc(cmd)}</code>" if cmd is not None else '<span class="muted">—</span>'))

    parse = '<span class="muted">—</span>'
    for c in calls:
        if c["fn"] == "prepare_statement":
            st = after(c, "statement")
            if st and st.get("type") == "STATEMENT_INSERT":
                r = st["row"]
                parse = (f"<code>Statement{{INSERT}}</code><br>row = ({r['id']}, "
                         f"{esc(r['username'])}, {esc(r['email'])})")
            elif st:
                parse = f"<code>Statement{{{esc(st['type'].replace('STATEMENT_', ''))}}}</code>"
            parse += f"<br><span class='muted'>→ {esc(c.get('ret', ''))}</span>"
        if c["fn"] == "do_meta_command":
            parse = f"meta command <code>{esc(cmd)}</code>"
    boxes.append(("Front end", "prepare_statement()", parse))

    exe = '<span class="muted">—</span>'
    for c in calls:
        if c["fn"] in ("table_end", "table_start") and c.get("ret_data"):
            cur = c["ret_data"]
            exe = (f"<code>{c['fn']}()</code> → cursor at page {cur['page_num']}, "
                   f"cell {cur['cell_num']}")
            if c["fn"] == "table_start":
                n = fns.count("print_row")
                exe += f"<br>walks the cells, {n} row(s) printed"
        if c["fn"] == "serialize_row":
            dest = next((d for n, _, d in c["args"] if n == "destination"), None)
            if dest and dest.get("page") is not None:
                exe += f"<br>row serialized into page {dest['page']} at byte {dest['offset']}"
        if c["fn"] == "db_close":
            exe = f"<code>db_close()</code> flushes {len(step['flushes'])} page(s)"
    boxes.append(("VM + cursor", "execute_statement()", exe))

    return "".join(
        f'<div class="box flow"><h4>{t}</h4><p class="sub"><code>{f}</code></p><div>{b}</div></div>'
        '<div class="arrow">→</div>'
        for t, f, b in boxes)


def render_call(c, docs):
    fn = c["fn"]
    sig, doc, line = docs.get(fn, (fn, "", 0))
    helper = " helper" if fn in HELPERS else ""
    args = []
    after_map = {n: t for n, t, _ in c.get("after", [])}
    for n, text, _ in c["args"]:
        a = f'<span class="an">{esc(n)}</span>=<span class="av">{esc(clip(text, 70))}</span>'
        if n in after_map and after_map[n] != text:
            a += f' <span class="chg">⟶ {esc(clip(after_map[n], 70))}</span>'
        args.append(a)
    ret = ""
    if c.get("noreturn"):
        ret = '<span class="ret muted">never returns (exit)</span>'
    elif c.get("ret") is not None:
        ret = f'<span class="ret">→ {esc(clip(c["ret"], 80))}</span>'
    notes = "".join(f'<div class="cnote">{esc(n)}</div>' for n in c["notes"])
    tip = esc(f"{sig}  (main.c:{line})\n\n{doc or 'no comment above this function'}")
    return (f'<div class="call{helper} {layer_class(fn)}" style="--d:{c["depth"]}">'
            f'<a class="fn" href="#fn-{fn}" data-tip="{tip}">{esc(fn)}</a>'
            f'<span class="args">({", ".join(args)})</span> {ret}{notes}</div>')


# ---------------------------------------------------------------- b-tree graph

ROOT_PAGE = 0  # table->root_page_num; the tutorial keeps the root at page 0
SLOT_W, FREE_W, NODE_H, PAD, GAP, LEVEL_H, TOP = 34, 54, 30, 6, 28, 96, 30


def step_pages(step):
    """(memory pages, disk pages, exited) as page -> node dicts, for drawing the tree."""
    snap = step["snap"]
    if not snap:
        return None
    exited = snap["label"] == "after exit"
    mem = {} if exited or not step["mem"] else {p["page"]: p for p in step["mem"]["pages"]}
    disk = {p["page"]: p for p in snap["disk"]["pages"]}
    return mem, disk, exited


def build_tree(mem, disk, page=ROOT_PAGE, seen=None):
    """Walk from the root, preferring the cached copy of each page over the file's."""
    seen = set() if seen is None else seen
    if page in seen:
        return None
    seen.add(page)
    if page in mem:
        node, where = mem[page], "mem"
    elif page in disk:
        node, where = disk[page], "disk"
    else:
        return None
    kids = []
    if node.get("kind") == "internal":
        kids = [build_tree(mem, disk, ch, seen) for ch in node["children"]]
    return {"page": page, "node": node, "where": where, "children": [k for k in kids if k]}


def node_keys(node):
    return node["keys"] if node.get("kind") == "internal" else [c["key"] for c in node["cells"]]


def tree_keys(t):
    if not t:
        return set()
    out = {(t["page"], k) for k in node_keys(t["node"])}
    for ch in t["children"]:
        out |= tree_keys(ch)
    return out


def node_width(t):
    n = t["node"]
    free = n.get("kind") != "internal" and n["num_cells"] < n.get("max_cells", 0)
    return max(70, PAD * 2 + len(node_keys(n)) * SLOT_W + (FREE_W if free else 0))


def layout(t, depth=0, x0=0.0):
    """Place each node centred over its children; returns the subtree's width."""
    t["w"], t["y"] = node_width(t), TOP + depth * LEVEL_H
    kids = [layout(ch, depth + 1, 0) for ch in t["children"]]  # widths first
    span = sum(kids) + GAP * (len(kids) - 1) if kids else 0
    total = max(t["w"], span)
    x = x0 + (total - span) / 2
    for ch, w in zip(t["children"], kids):
        shift(ch, x - ch["_x0"])
        x += w + GAP
    t["x"], t["_x0"] = x0 + (total - t["w"]) / 2, x0
    return total


def shift(t, dx):
    t["x"] += dx
    t["_x0"] += dx
    for ch in t["children"]:
        shift(ch, dx)


def depth_of(t):
    return 1 + max((depth_of(c) for c in t["children"]), default=0)


def draw_node(t, new_keys, cursor, mini, out):
    n, x, y = t["node"], t["x"], t["y"]
    internal = n.get("kind") == "internal"
    keys = node_keys(n)
    where = "" if t["where"] == "mem" else " disk"
    kind = "internal" if internal else "leaf"
    if not mini:
        count = f"{n['num_keys']} keys" if internal else f"{n['num_cells']}/{n.get('max_cells', '?')} cells"
        src = "in memory" if t["where"] == "mem" else "on disk only"
        out.append(f'<text class="bt-label" x="{x}" y="{y - 8}">page {t["page"]} · {kind} · '
                   f'{count} · {src}</text>')
    tip = esc(f"page {t['page']} ({kind}), {'cached in pager->pages[]' if not where else 'not loaded yet: only in the file'}")
    out.append(f'<rect class="bt-node {kind}{where}" x="{x}" y="{y}" width="{t["w"]}" '
               f'height="{NODE_H}" rx="6" data-tip="{tip}"/>')
    for i, k in enumerate(keys):
        sx = x + PAD + i * SLOT_W
        cls = " new" if (t["page"], k) in new_keys else ""
        if internal:
            tip = f"key {k}: children left of it hold keys ≤ {k}"
        else:
            c = n["cells"][i]
            tip = (f"cell {i} · key {k} → row ({c['id']}, {c['username']}, {c['email']}) · "
                   f"page {t['page']} byte {c['offset']}")
        out.append(f'<g data-tip="{esc(tip)}"><rect class="bt-slot{cls}" x="{sx}" y="{y + 4}" '
                   f'width="{SLOT_W - 2}" height="{NODE_H - 8}" rx="3"/>'
                   f'<text class="bt-key{cls}" x="{sx + (SLOT_W - 2) / 2}" y="{y + NODE_H / 2 + 4}">{k}</text></g>')
    if not internal and n["num_cells"] < n.get("max_cells", 0):
        fx = x + PAD + len(keys) * SLOT_W
        free = n["max_cells"] - n["num_cells"]
        out.append(f'<text class="bt-free" x="{fx + FREE_W / 2 - 2}" y="{y + NODE_H / 2 + 4}" '
                   f'data-tip="{free} empty cell slot(s) before this leaf is full">+{free} free</text>')
    if cursor and cursor["page_num"] == t["page"] and not mini:
        cx = x + PAD + cursor["cell_num"] * SLOT_W + (SLOT_W - 2) / 2
        by = y + NODE_H
        out.append(f'<path class="bt-cursor" d="M{cx} {by + 3} l-6 9 h12 z"/>'
                   f'<text class="bt-cursor-label" x="{cx - 8}" y="{by + 24}">{esc(cursor["label"])} '
                   f'(cell {cursor["cell_num"]})</text>')
    for i, ch in enumerate(t["children"]):
        ex = x + PAD + i * SLOT_W - 1
        out.append(f'<line class="bt-edge" x1="{ex}" y1="{y + NODE_H}" '
                   f'x2="{ch["x"] + ch["w"] / 2}" y2="{ch["y"] - (4 if mini else 22)}"/>')
        draw_node(ch, new_keys, cursor, mini, out)


def step_cursor(step):
    """The cursor this step positioned: where an insert lands, or where a scan starts."""
    for c in step["calls"]:
        if c["fn"] in ("table_end", "table_start") and c.get("ret_data"):
            label = "insert here" if c["fn"] == "table_end" else "scan starts"
            return dict(c["ret_data"], label=f"{c['fn']}(): {label}")
    return None


def render_tree(tree, new_keys, cursor=None, mini=False):
    if tree is None:
        return '<p class="muted small">no root page yet</p>'
    width = layout(tree)
    height = TOP + (depth_of(tree) - 1) * LEVEL_H + NODE_H + (8 if mini else 34)
    out = []
    draw_node(tree, new_keys, cursor, mini, out)
    top = 0 if not mini else TOP - 6
    return (f'<svg class="bt{" mini" if mini else ""}" viewBox="-4 {top} {width + 8} {height - top}" '
            f'width="{width + 8}" height="{height - top}" role="img" '
            f'aria-label="B-tree rooted at page {tree["page"]}">{"".join(out)}</svg>')


def tree_caption(tree, exited):
    if tree is None:
        return ""
    src = {tree["where"]}
    stack = list(tree["children"])
    while stack:
        t = stack.pop()
        src.add(t["where"])
        stack += t["children"]
    if exited:
        return "process exited: tree drawn from the file"
    if src == {"disk"}:
        return "no page cached yet: tree drawn from the file (dashed = on disk only)"
    return "tree drawn from the pager's cache in memory (dashed nodes would be on disk only)"



def render_step(step, idx, prev_mem, prev_disk, prev_keys, docs, anchor):
    if idx == 0:
        title = "Startup: <code>db_open()</code> before the first prompt"
    elif step["command"] is None:
        title = "End of input: <code>read_input()</code> hits EOF and exits"
    else:
        title = f"Command {idx}: <code>{esc(step['command'])}</code>"
    snap = step["snap"]
    mem = step["mem"] if not (snap and snap["label"] == "after exit") else None
    disk = snap["disk"] if snap else {"exists": False, "size": 0, "pages": []}
    out = step["output"].rstrip("\n")
    output = f'<pre class="out">{esc(out)}</pre>' if out else ""

    flush = ""
    if step["flushes"]:
        items = "".join(f"<li>after flush {i + 1}: file is {f['disk']['size']} B</li>"
                        for i, f in enumerate(step["flushes"]))
        flush = f'<ul class="flushes">{items}</ul>'

    store = ""
    if snap:
        disk_sub = (f"{DB} · {disk['size']} B · {len(disk['pages'])} page(s)"
                    if disk["exists"] else f"{DB} does not exist yet")
        mem_sub = (f"pager->pages[] · num_pages = {mem['num_pages']} · "
                   f"{len(mem['pages'])} page(s) cached" if mem else "")
        store = (f'{render_store("Page cache (memory)", mem and mem["pages"], disk["pages"], mem_sub, prev_mem)}'
                 f'<div class="arrow">⇢</div>'
                 f'{render_store("File on disk", disk["pages"], None, disk_sub, prev_disk)}')
        store = f'<div class="stores">{store}</div>{sync_badge(mem, disk)}{flush}'

    calls = "".join(render_call(c, docs) for c in step["calls"])
    n_calls = len(step["calls"])
    n_helpers = sum(c["fn"] in HELPERS for c in step["calls"])
    pages = step_pages(step)
    graph = ""
    if pages:
        tree = build_tree(pages[0], pages[1])
        new = tree_keys(tree) - prev_keys
        graph = (f'<div class="btree"><h4>B-tree after this step</h4>'
                 f'<div class="btwrap">{render_tree(tree, new, step_cursor(step))}</div>'
                 f'<p class="sub">{tree_caption(tree, pages[2])}'
                 f'{" · highlighted = keys added in this step" if new else ""}</p></div>')
    return f"""
<section class="card step" id="{anchor}">
  <h3>{title}</h3>
  {output}
  {graph}
  <div class="flowrow">{flow_boxes(step) if idx else ''}{store and f'<div class="storewrap">{store}</div>'}</div>
  <details {'open' if idx else ''}><summary>Call tree: {n_calls} call(s)
    <span class="muted">({n_helpers} low-level helper call(s) hidden unless “show helpers” is on)</span></summary>
    <div class="tree">{calls}</div>
  </details>
</section>"""


def render_reference(docs, counts):
    groups = []
    seen = set()
    for i, (name, fns) in enumerate(LAYERS + [("Other", [f for f in docs if f not in LAYER_OF])]):
        items = []
        for fn in fns:
            if fn not in docs or fn in seen:
                continue
            seen.add(fn)
            sig, doc, line = docs[fn]
            n = counts.get(fn, 0)
            items.append(f'<div class="ref L{i}" id="fn-{fn}"><div><code>{esc(sig)}</code>'
                         f'<span class="muted small"> · main.c:{line} · called {n}×</span></div>'
                         f'<p>{esc(doc) or "<i>no comment</i>"}</p></div>')
        if items:
            groups.append(f'<div class="group"><h3><i class="sw L{i}"></i>{name}</h3>{"".join(items)}</div>')
    return "".join(groups)


def render(sessions, traces, docs):
    counts = {}
    for t in traces:
        for e in t["events"]:
            if e["type"] == "call":
                counts[e["fn"]] = counts.get(e["fn"], 0) + 1
    body = []
    previous_disk, prev_keys = [], set()
    for n, (cmds, trace) in enumerate(zip(sessions, traces), 1):
        steps = split_steps(trace)
        prev_mem, prev_disk = None, previous_disk
        parts, frames = [], []
        for i, step in enumerate(steps):
            anchor = f"s{n}-step{i}"
            parts.append(render_step(step, i, prev_mem and prev_mem["pages"], prev_disk,
                                     prev_keys, docs, anchor))
            pages = step_pages(step)
            if pages:
                tree = build_tree(pages[0], pages[1])
                keys = tree_keys(tree)
                label = "startup" if i == 0 else (step["command"] or "end of input")
                frames.append(f'<a class="frame" href="#{anchor}"><div class="fimg">'
                              f'{render_tree(tree, keys - prev_keys, mini=True)}</div>'
                              f'<code>{esc(clip(label, 28))}</code>'
                              f'<span class="muted small">{len(keys)} key(s)</span></a>')
                prev_keys = keys
            if step["mem"]:
                prev_mem = step["mem"]
            if step["snap"]:
                prev_disk = step["snap"]["disk"]["pages"]
        previous_disk = prev_disk
        strip = ('<section class="card"><h3>B-tree over this session '
                 '<span class="muted small">· one frame per command, highlighted = keys added; '
                 'click a frame to jump to it</span></h3>'
                 f'<div class="strip">{"<span class=arrow>→</span>".join(frames)}</div></section>')
        again = " (restarted on the same file)" if n > 1 else ""
        body.append(f'<h2 class="session">Session {n}: <code>./main {DB}</code>{again}</h2>'
                    f'<pre class="out transcript">{esc(trace["stdout"])}</pre>{strip}{"".join(parts)}')
    legend = "".join(f'<span class="key"><i class="sw L{i}"></i>{name}</span>'
                     for i, (name, _) in enumerate(LAYERS))
    return TEMPLATE.format(legend=legend, reference=render_reference(docs, counts),
                           sessions="".join(body), db=DB,
                           nsessions=len(sessions), ncalls=sum(counts.values()))


TEMPLATE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>main.c execution trace</title>
<style>
.viz-root {{
  color-scheme: light;
  --page: #f9f9f7; --surface: #fcfcfb; --ink: #0b0b0b; --ink-2: #52514e; --muted: #898781;
  --grid: #e1e0d9; --ring: rgba(11,11,11,0.10); --wash: rgba(42,120,214,0.10);
  --warn: #fab219; --good: #0ca30c;
  --L0: #2a78d6; --L1: #eb6834; --L2: #1baf7a; --L3: #eda100; --L4: #e87ba4; --L5: #008300;
  --L6: #4a3aa7; --L7: #898781;
}}
@media (prefers-color-scheme: dark) {{
  :root:where(:not([data-theme="light"])) .viz-root {{
    color-scheme: dark;
    --page: #0d0d0d; --surface: #1a1a19; --ink: #ffffff; --ink-2: #c3c2b7; --muted: #898781;
    --grid: #2c2c2a; --ring: rgba(255,255,255,0.10); --wash: rgba(57,135,229,0.18);
    --L0: #3987e5; --L1: #d95926; --L2: #199e70; --L3: #c98500; --L4: #d55181; --L5: #008300;
    --L6: #9085e9; --L7: #898781;
  }}
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; }}
.viz-root {{ background: var(--page); color: var(--ink); min-height: 100vh; padding: 32px;
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }}
main {{ max-width: 1280px; margin: 0 auto; }}
h1 {{ font-size: 22px; margin: 0 0 4px; }}
h2 {{ font-size: 18px; margin: 32px 0 8px; }}
h3 {{ font-size: 15px; margin: 0 0 10px; }}
h4 {{ font-size: 13px; margin: 0; }}
code {{ font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; }}
.muted {{ color: var(--muted); font-weight: 400; }}
.small {{ font-size: 12px; }}
.card {{ background: var(--surface); border: 1px solid var(--ring); border-radius: 10px; padding: 18px; margin: 14px 0; }}
.toolbar {{ position: sticky; top: 0; z-index: 5; background: var(--page); padding: 10px 0;
  display: flex; gap: 20px; align-items: center; flex-wrap: wrap; border-bottom: 1px solid var(--grid); }}
.legend {{ display: flex; flex-wrap: wrap; gap: 4px 14px; color: var(--ink-2); font-size: 12px; }}
.key {{ display: inline-flex; align-items: center; gap: 6px; }}
.sw {{ width: 10px; height: 10px; border-radius: 3px; display: inline-block; margin-right: 6px; background: var(--c); }}
.L0 {{ --c: var(--L0); }} .L1 {{ --c: var(--L1); }} .L2 {{ --c: var(--L2); }} .L3 {{ --c: var(--L3); }}
.L4 {{ --c: var(--L4); }} .L5 {{ --c: var(--L5); }} .L6 {{ --c: var(--L6); }} .L7 {{ --c: var(--L7); }}
.pipeline {{ display: flex; flex-wrap: wrap; align-items: center; gap: 6px; margin-top: 8px; }}
.pipeline span {{ border-left: 4px solid var(--c); padding: 4px 10px; background: var(--surface);
  border-radius: 6px; border-top: 1px solid var(--ring); border-right: 1px solid var(--ring); border-bottom: 1px solid var(--ring); }}
.ref {{ border-left: 3px solid var(--c); padding: 4px 12px; margin: 6px 0; }}
.ref p {{ margin: 2px 0 0; color: var(--ink-2); }}
.ref:target {{ background: var(--wash); }}
.group h3 {{ margin: 16px 0 4px; display: flex; align-items: center; }}
.refgrid {{ columns: 2 480px; column-gap: 28px; }}
.group {{ break-inside: avoid-column; }}
.session {{ border-top: 1px solid var(--grid); padding-top: 20px; }}
pre.out {{ font: 12px/1.5 ui-monospace, Menlo, monospace; background: var(--page); border: 1px solid var(--ring);
  border-radius: 6px; padding: 8px 12px; margin: 0 0 12px; white-space: pre-wrap; color: var(--ink-2); }}
.transcript {{ max-width: 620px; }}
.flowrow {{ display: flex; flex-wrap: wrap; align-items: stretch; gap: 6px; }}
.box {{ border: 1px solid var(--ring); border-radius: 8px; padding: 10px 12px; background: var(--page); }}
.box.flow {{ flex: 1 1 170px; max-width: 250px; font-size: 13px; }}
.box .sub {{ margin: 0 0 6px; color: var(--muted); font-size: 11px; }}
.arrow {{ align-self: center; color: var(--muted); font-size: 18px; padding: 0 2px; }}
.storewrap {{ flex: 3 1 520px; }}
.stores {{ display: flex; gap: 6px; }}
.box.store {{ flex: 1 1 0; min-width: 0; }}
.pg {{ margin-top: 4px; font-size: 12px; }}
table.cells {{ border-collapse: collapse; width: 100%; font-size: 12px; margin-top: 4px; font-variant-numeric: tabular-nums; }}
.cells th, .cells td {{ text-align: left; padding: 2px 8px 2px 0; border-bottom: 1px solid var(--grid); }}
.cells th {{ color: var(--muted); font-weight: 600; }}
.cells tr.new td {{ background: var(--wash); font-weight: 600; }}
.badge {{ display: inline-block; margin-top: 8px; font-size: 12px; padding: 3px 10px; border-radius: 999px;
  border: 1px solid var(--ring); color: var(--ink); }}
.badge.warn {{ border-color: var(--warn); }}
.badge.ok {{ border-color: var(--good); }}
.flushes {{ margin: 6px 0 0; font-size: 12px; color: var(--ink-2); }}
details {{ margin-top: 14px; }}
summary {{ cursor: pointer; font-weight: 600; font-size: 13px; }}
.tree {{ margin-top: 8px; font: 12px/1.45 ui-monospace, Menlo, monospace; }}
.call {{ padding: 2px 0 2px calc(8px + (var(--d) - 1) * 20px); border-left: 3px solid var(--c); }}
.call:hover {{ background: var(--wash); }}
.call.helper {{ display: none; opacity: 0.6; }}
.show-helpers .call.helper {{ display: block; }}
.fn {{ color: var(--ink); font-weight: 700; text-decoration: none; }}
.fn:hover {{ text-decoration: underline; }}
.an {{ color: var(--muted); }}
.av {{ color: var(--ink-2); }}
.chg {{ color: var(--ink); background: var(--wash); border-radius: 3px; padding: 0 3px; }}
.ret {{ color: var(--ink); }}
.cnote {{ font-family: system-ui, sans-serif; color: var(--ink-2); padding-left: 16px; }}
.cnote::before {{ content: "↳ "; color: var(--muted); }}
.btree {{ margin: 4px 0 14px; }}
.btwrap {{ overflow-x: auto; padding: 6px 0; }}
.btree .sub {{ color: var(--muted); font-size: 12px; margin: 2px 0 0; }}
svg.bt {{ display: block; max-width: 100%; height: auto; overflow: visible; }}
.bt-node {{ fill: var(--surface); stroke: var(--L4); stroke-width: 2; }}
.bt-node.internal {{ stroke: var(--L6); }}
.bt-node.disk {{ stroke-dasharray: 5 4; fill: var(--page); }}
.bt-slot {{ fill: var(--page); stroke: var(--grid); }}
.bt-slot.new {{ fill: var(--wash); stroke: var(--ink-2); }}
.bt-key {{ fill: var(--ink); font: 12px ui-monospace, Menlo, monospace; text-anchor: middle; }}
.bt-key.new {{ font-weight: 700; }}
.bt-free {{ fill: var(--muted); font-size: 11px; text-anchor: middle; }}
.bt-label {{ fill: var(--ink-2); font-size: 11px; }}
.bt-edge {{ stroke: var(--muted); stroke-width: 1.5; }}
.bt-cursor {{ fill: var(--ink); }}
.bt-cursor-label {{ fill: var(--ink); font-size: 11px; }}
.strip {{ display: flex; align-items: center; gap: 8px; overflow-x: auto; padding-bottom: 6px; }}
.frame {{ flex: 0 0 auto; display: flex; flex-direction: column; align-items: center; gap: 4px;
  padding: 8px 10px; border: 1px solid var(--ring); border-radius: 8px; background: var(--page);
  color: var(--ink); text-decoration: none; min-width: 120px; }}
.frame:hover {{ background: var(--wash); }}
.fimg svg.mini {{ height: 44px; width: auto; max-width: 260px; }}
#tip {{ position: fixed; pointer-events: none; background: var(--surface); color: var(--ink); white-space: pre-wrap;
  border: 1px solid var(--ring); border-radius: 6px; padding: 8px 10px; font-size: 12px;
  box-shadow: 0 4px 16px rgba(0,0,0,0.15); max-width: 460px; display: none; z-index: 10; }}
</style></head>
<body><div class="viz-root"><main>
  <h1>How <code>main.c</code> turns input into bytes in <code>{db}</code></h1>
  <div class="muted">A real run under gdb: {nsessions} session(s), {ncalls} function calls recorded.
    Generated by <code>make trace</code>.</div>

  <section class="card">
    <h3>The path every command takes</h3>
    <div class="pipeline">
      <span class="L0">Input · read_input</span>→<span class="L1">Front end · prepare_statement</span>→
      <span class="L2">VM · execute_*</span>→<span class="L3">Cursor · table_end / table_start</span>→
      <span class="L4">Node · leaf_node_insert, serialize_row</span>→<span class="L5">Pager · get_page (memory)</span>→
      <span class="L6">.exit · db_close → pager_flush (disk)</span>
    </div>
    <p class="muted small">Rows are written into a 4 KB page that lives in memory, in the pager's cache.
      The file only changes when <code>.exit</code> flushes each cached page. Watch the
      “Page cache” and “File on disk” boxes in each step below.</p>
  </section>

  <div class="toolbar">
    <label><input type="checkbox" id="helpers"> show low-level helpers (leaf_node_cell, _key, _value, _num_cells)</label>
    <div class="legend">{legend}</div>
  </div>

  {sessions}

  <h2>What each function does</h2>
  <p class="muted small">Taken from the comment above each function in main.c. Hover a function in any call tree to see this; click it to jump here.</p>
  <section class="card refgrid">{reference}</section>
</main><div id="tip" role="tooltip"></div></div>
<script>
document.getElementById("helpers").addEventListener("change", e =>
  document.body.classList.toggle("show-helpers", e.target.checked));
const tip = document.getElementById("tip");
document.addEventListener("mousemove", e => {{
  const t = e.target.closest("[data-tip]");
  if (!t) {{ tip.style.display = "none"; return; }}
  tip.textContent = t.dataset.tip;
  tip.style.display = "block";
  tip.style.left = Math.min(e.clientX + 14, innerWidth - tip.offsetWidth - 8) + "px";
  const y = e.clientY + 16;
  tip.style.top = (y + tip.offsetHeight > innerHeight ? e.clientY - tip.offsetHeight - 10 : y) + "px";
}});
</script>
</body></html>
"""


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else None
    sessions = read_sessions(path)
    docs = function_docs(open("main.c").read())
    os.makedirs(OUT_DIR, exist_ok=True)
    if os.path.exists(DB):
        os.remove(DB)
    with tempfile.TemporaryDirectory() as tmp:
        traces = [run_session(cmds, tmp, n) for n, cmds in enumerate(sessions, 1)]
    with open(OUT, "w") as f:
        f.write(render(sessions, traces, docs))
    print(f"wrote {OUT} ({len(sessions)} session(s))")


if __name__ == "__main__":
    main()
