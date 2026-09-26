#!/usr/bin/env python3
"""Render a database file produced by ./main as a self-contained HTML page.

Usage: python3 tools/visualize.py <db file> <out.html>

The page shows the REPL's layers, then every page of the file as a B-tree leaf
node: a byte-layout bar, a table of its cells and a colour-coded hex dump.

Node-layout sizes come from running `./main <db> .constants`, so they follow
main.c. The Row's field widths are not printed by .constants and are mirrored
below; they are checked against ROW_SIZE so a change to Row fails loudly here.
"""

import html
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# Row layout, mirrored from main.c (ID_SIZE, USERNAME_SIZE, EMAIL_SIZE).
ID_SIZE = 4
USERNAME_SIZE = 32 + 1
EMAIL_SIZE = 255 + 1
PAGE_SIZE = 4096

# Region -> (label, description). Order is the categorical slot order in the CSS.
REGIONS = {
    "common": ("common header", "node type (1 B), is_root (1 B), parent pointer (4 B)"),
    "ncells": ("num_cells", "how many cells this leaf holds (uint32)"),
    "key": ("key", "cell key (uint32): the row id"),
    "id": ("row.id", "serialized Row: id (uint32)"),
    "user": ("row.username", "serialized Row: username, 32 chars + NUL"),
    "email": ("row.email", "serialized Row: email, 255 chars + NUL"),
    "free": ("unused", "space not holding a cell yet"),
}


def read_constants(db_path):
    """Run `.constants` against a copy of the db, so the real file is not rewritten."""
    with tempfile.TemporaryDirectory() as tmp:
        copy = os.path.join(tmp, "copy.db")
        shutil.copyfile(db_path, copy)
        out = subprocess.run(
            ["./main", copy], input=".constants\n.exit\n",
            capture_output=True, text=True, check=True,
        ).stdout
    consts = {}
    for line in out.splitlines():
        line = line.replace("db > ", "")
        if ":" in line:
            name, _, value = line.partition(":")
            if value.strip().isdigit():
                consts[name.strip()] = int(value)
    row = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE
    if consts.get("ROW_SIZE") != row:
        sys.exit(f"ROW_SIZE is {consts.get('ROW_SIZE')} but visualize.py expects {row}; "
                 "update the Row field sizes at the top of tools/visualize.py")
    return consts


def cstr(raw):
    """Decode a NUL-terminated column."""
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def parse_page(page, c):
    """Split one page into its header fields, cells and a byte -> region map."""
    hdr = c["LEAF_NODE_HEADER_SIZE"]
    common = c["COMMON_NODE_HEADER_SIZE"]
    cell_size = c["LEAF_NODE_CELL_SIZE"]
    node_type, is_root = page[0], page[1]
    parent, num_cells = struct.unpack_from("<II", page, 2)
    shown = min(num_cells, c["LEAF_NODE_MAX_CELLS"])

    regions = [("common", 0, common), ("ncells", common, hdr)]
    cells = []
    for i in range(shown):
        start = hdr + i * cell_size
        key, = struct.unpack_from("<I", page, start)
        v = start + 4
        rid, = struct.unpack_from("<I", page, v)
        user = cstr(page[v + ID_SIZE: v + ID_SIZE + USERNAME_SIZE])
        email = cstr(page[v + ID_SIZE + USERNAME_SIZE: v + ID_SIZE + USERNAME_SIZE + EMAIL_SIZE])
        cells.append(dict(i=i, start=start, key=key, id=rid, user=user, email=email))
        regions += [
            ("key", start, v),
            ("id", v, v + ID_SIZE),
            ("user", v + ID_SIZE, v + ID_SIZE + USERNAME_SIZE),
            ("email", v + ID_SIZE + USERNAME_SIZE, start + cell_size),
        ]
    end = hdr + shown * cell_size
    regions.append(("free", end, PAGE_SIZE))
    return dict(node_type=node_type, is_root=is_root, parent=parent,
                num_cells=num_cells, cells=cells, regions=regions)


def region_tip(kind, a, b, cell=None):
    label, desc = REGIONS[kind]
    where = f"cell {cell}: " if cell is not None else ""
    return f"{where}{label} · bytes {a}–{b - 1} ({b - a} B) · {desc}"


def render_bar(p):
    out = []
    cell = -1
    for kind, a, b in p["regions"]:
        if kind == "key":
            cell += 1
        tip = region_tip(kind, a, b, cell if kind in ("key", "id", "user", "email") else None)
        # flex-grow by byte count, so the 2px gaps come out of the whole bar evenly
        out.append(f'<span class="seg r-{kind}" style="flex:{b - a} 1 0" '
                   f'data-tip="{html.escape(tip)}"></span>')
    return '<div class="bar">' + "".join(out) + "</div>"


def render_hex(page, p, c):
    """Hex dump of the header plus the first cell, each byte coloured by region."""
    limit = c["LEAF_NODE_HEADER_SIZE"] + (c["LEAF_NODE_CELL_SIZE"] if p["cells"] else 16)
    owner = ["free"] * PAGE_SIZE
    for kind, a, b in p["regions"]:
        for i in range(a, b):
            owner[i] = kind
    rows = []
    skipped = 0
    for off in range(0, limit, 16):
        line = page[off:min(off + 16, limit)]
        # Collapse full all-zero lines (mostly NUL padding of the string columns).
        if len(line) == 16 and not any(line) and off + 16 < limit:
            skipped += 1
            continue
        if skipped:
            rows.append(f'<div class="hexrow gap">… {skipped} all-zero line(s), '
                        f'{skipped * 16} B of NUL padding …</div>')
            skipped = 0
        hexes, chars = [], []
        for i in range(off, min(off + 16, limit)):
            byte = page[i]
            k = owner[i]
            zero = " zero" if byte == 0 else ""
            tip = html.escape(f"byte {i} = 0x{byte:02x} · {REGIONS[k][0]}")
            hexes.append(f'<span class="hx t-{k}{zero}" data-tip="{tip}">{byte:02x}</span>')
            ch = chr(byte) if 32 <= byte < 127 else "·"
            chars.append(f'<span class="t-{k}{zero}">{html.escape(ch)}</span>')
        rows.append(f'<div class="hexrow"><span class="off">{off:08x}</span>'
                    f'<span class="hexes">{"".join(hexes)}</span>'
                    f'<span class="chars">{"".join(chars)}</span></div>')
    return "".join(rows)


def render_page(num, page, c):
    p = parse_page(page, c)
    keys = [cell["key"] for cell in p["cells"]]
    notes = []
    if keys != sorted(keys):
        notes.append("Keys are in insertion order, not sorted: execute_insert always "
                     "writes at table_end(). Sorted insert comes in the next tutorial part.")
    if p["node_type"] == 0:
        notes.append("Node type byte is 0 (NODE_INTERNAL) even though this is a leaf: "
                     "initialize_leaf_node() only zeroes num_cells so far.")
    used = c["LEAF_NODE_HEADER_SIZE"] + len(p["cells"]) * c["LEAF_NODE_CELL_SIZE"]

    rows = "".join(
        f"<tr><td>{x['i']}</td><td>{x['start']}</td><td>{x['key']}</td><td>{x['id']}</td>"
        f"<td>{html.escape(x['user'])}</td><td>{html.escape(x['email'])}</td></tr>"
        for x in p["cells"]
    ) or '<tr><td colspan="6" class="muted">no cells</td></tr>'

    return f"""
<section class="card">
  <h2>Page {num} <span class="muted">· offset {num * PAGE_SIZE} in the file</span></h2>
  <div class="stats">
    <div><p><b>{p['num_cells']}</b> / {c['LEAF_NODE_MAX_CELLS']}</p><span>cells</span></div>
    <div><p><b>{used}</b> / {PAGE_SIZE}</p><span>bytes used</span></div>
    <div><b>{p['node_type']}</b><span>node type</span></div>
    <div><b>{p['is_root']}</b><span>is_root</span></div>
    <div><b>{p['parent']}</b><span>parent</span></div>
  </div>
  <h3>Byte layout (4096 B, hover a segment)</h3>
  {render_bar(p)}
  <div class="axis"><span>0</span><span>1024</span><span>2048</span><span>3072</span><span>4096</span></div>
  {''.join(f'<p class="note">{html.escape(n)}</p>' for n in notes)}
  <h3>Cells: what <code>.btree</code> and <code>select</code> see</h3>
  <table><thead><tr><th>cell</th><th>byte offset</th><th>key</th><th>row.id</th>
  <th>row.username</th><th>row.email</th></tr></thead><tbody>{rows}</tbody></table>
  <h3>Raw bytes: header + first cell</h3>
  <div class="hex">{render_hex(page, p, c)}</div>
</section>"""


LAYERS = [
    ("Input", "read_input()", "one line from stdin"),
    ("Front end", "do_meta_command() · prepare_statement()", "text → Statement; no data touched"),
    ("Virtual machine", "execute_insert() · execute_select()", "talks only to a Cursor"),
    ("Cursor", "table_start() · table_end() · cursor_advance() · cursor_value()",
     "a finger on (page_num, cell_num)"),
    ("B-tree node", "leaf_node_cell() · leaf_node_key() · leaf_node_value() · leaf_node_insert()",
     "pointer arithmetic inside one page"),
    ("Pager", "get_page() · pager_flush()", "cache of 4 KB pages, flushed on .exit"),
    ("File", "db_open() · db_close()", "a whole number of 4096-byte pages"),
]


def render(db_path, data, c):
    pages = [data[i:i + PAGE_SIZE] for i in range(0, len(data), PAGE_SIZE)]
    layers = "".join(
        f'<div class="layer"><b>{n}</b><code>{html.escape(f)}</code><span>{d}</span></div>'
        for n, f, d in LAYERS
    )
    legend = "".join(
        f'<span class="key"><i class="sw r-{k}"></i>{label}</span>'
        for k, (label, _) in REGIONS.items()
    )
    consts = "".join(f"<tr><td><code>{k}</code></td><td>{v}</td></tr>" for k, v in c.items())
    body = "".join(render_page(i, pg, c) for i, pg in enumerate(pages)) or \
        '<section class="card"><p class="muted">The file has no pages yet.</p></section>'
    return TEMPLATE.format(
        title=html.escape(os.path.basename(db_path)),
        size=len(data), npages=len(pages), layers=layers, legend=legend,
        consts=consts, body=body,
    )


TEMPLATE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} · db visualizer</title>
<style>
.viz-root {{
  color-scheme: light;
  --page: #f9f9f7; --surface: #fcfcfb; --ink: #0b0b0b; --ink-2: #52514e; --muted: #898781;
  --grid: #e1e0d9; --ring: rgba(11,11,11,0.10);
  --c-common: #2a78d6; --c-ncells: #eb6834; --c-key: #1baf7a; --c-id: #eda100;
  --c-user: #e87ba4; --c-email: #008300; --c-free: #e1e0d9;
}}
@media (prefers-color-scheme: dark) {{
  :root:where(:not([data-theme="light"])) .viz-root {{
    color-scheme: dark;
    --page: #0d0d0d; --surface: #1a1a19; --ink: #ffffff; --ink-2: #c3c2b7; --muted: #898781;
    --grid: #2c2c2a; --ring: rgba(255,255,255,0.10);
    --c-common: #3987e5; --c-ncells: #d95926; --c-key: #199e70; --c-id: #c98500;
    --c-user: #d55181; --c-email: #008300; --c-free: #2c2c2a;
  }}
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; }}
.viz-root {{ background: var(--page); color: var(--ink); min-height: 100vh; padding: 32px;
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }}
main {{ max-width: 1100px; margin: 0 auto; }}
h1 {{ font-size: 22px; margin: 0 0 4px; }}
h2 {{ font-size: 17px; margin: 0 0 12px; }}
h3 {{ font-size: 13px; color: var(--ink-2); margin: 20px 0 8px; font-weight: 600; }}
.muted {{ color: var(--muted); font-weight: 400; }}
code {{ font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; }}
.card {{ background: var(--surface); border: 1px solid var(--ring); border-radius: 10px;
  padding: 20px; margin: 16px 0; }}
.grid2 {{ display: grid; grid-template-columns: 3fr 2fr; gap: 16px; }}
@media (max-width: 800px) {{ .grid2 {{ grid-template-columns: 1fr; }} }}
.layer {{ display: grid; grid-template-columns: 130px 1fr; gap: 0 12px; padding: 8px 12px;
  border: 1px solid var(--ring); border-radius: 8px; position: relative; margin-bottom: 14px; }}
.layer:not(:last-child)::after {{ content: "▼"; position: absolute; left: 50%; bottom: -15px;
  font-size: 10px; color: var(--muted); }}
.layer code {{ color: var(--ink); }}
.layer span {{ grid-column: 2; color: var(--ink-2); font-size: 12px; }}
table {{ border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; }}
th, td {{ text-align: left; padding: 4px 10px 4px 0; border-bottom: 1px solid var(--grid); }}
th {{ color: var(--ink-2); font-weight: 600; font-size: 12px; }}
.stats {{ display: flex; gap: 28px; flex-wrap: wrap; }}
.stats div {{ display: flex; flex-direction: column; }}
.stats p {{ margin: 0; color: var(--ink-2); }}
.stats b {{ font-size: 20px; color: var(--ink); }}
.stats span {{ color: var(--muted); font-size: 12px; }}
.legend {{ display: flex; flex-wrap: wrap; gap: 6px 16px; margin: 8px 0 0; color: var(--ink-2); font-size: 12px; }}
.key {{ display: inline-flex; align-items: center; gap: 6px; }}
.sw {{ width: 12px; height: 12px; border-radius: 3px; display: inline-block; }}
.bar {{ display: flex; gap: 2px; height: 36px; }}
.seg {{ min-width: 2px; border-radius: 4px; cursor: default; }}
.seg:hover {{ outline: 2px solid var(--ink); outline-offset: 1px; }}
.r-common {{ background: var(--c-common); }} .r-ncells {{ background: var(--c-ncells); }}
.r-key {{ background: var(--c-key); }} .r-id {{ background: var(--c-id); }}
.r-user {{ background: var(--c-user); }} .r-email {{ background: var(--c-email); }}
.r-free {{ background: var(--c-free); }}
.axis {{ display: flex; justify-content: space-between; color: var(--muted); font-size: 11px;
  margin-top: 4px; font-variant-numeric: tabular-nums; }}
.note {{ border-left: 3px solid var(--muted); padding: 2px 10px; color: var(--ink-2); margin: 12px 0 0; }}
.hex {{ font: 12px/1.7 ui-monospace, SFMono-Regular, Menlo, monospace; overflow-x: auto; }}
.hexrow {{ display: flex; gap: 16px; white-space: pre; }}
.off {{ color: var(--muted); }}
.hexrow.gap {{ color: var(--muted); font-style: italic; padding-left: 10ch; }}
.hexes {{ display: inline-grid; grid-template-columns: repeat(16, 2.4ch); }}
.hx {{ cursor: default; }}
.t-common {{ color: var(--c-common); }} .t-ncells {{ color: var(--c-ncells); }}
.t-key {{ color: var(--c-key); }} .t-id {{ color: var(--c-id); }}
.t-user {{ color: var(--c-user); }} .t-email {{ color: var(--c-email); }}
.t-free {{ color: var(--muted); }}
.zero {{ opacity: 0.35; }}
.hx.t-common, .hx.t-ncells, .hx.t-key, .hx.t-id, .hx.t-user, .hx.t-email {{ font-weight: 700; }}
#tip {{ position: fixed; pointer-events: none; background: var(--surface); color: var(--ink);
  border: 1px solid var(--ring); border-radius: 6px; padding: 6px 10px; font-size: 12px;
  box-shadow: 0 4px 16px rgba(0,0,0,0.15); max-width: 360px; display: none; z-index: 10; }}
</style></head>
<body><div class="viz-root"><main>
  <h1>{title}</h1>
  <div class="muted">{size} bytes · {npages} page(s) · generated by <code>make viz</code></div>

  <div class="grid2">
    <section class="card">
      <h2>How one line flows through <code>main.c</code></h2>
      {layers}
    </section>
    <section class="card">
      <h2>Constants <span class="muted">· from <code>.constants</code></span></h2>
      <table><tbody>{consts}</tbody></table>
      <h3>Colour key</h3>
      <div class="legend">{legend}</div>
    </section>
  </div>

  {body}
</main><div id="tip" role="tooltip"></div></div>
<script>
const tip = document.getElementById("tip");
document.addEventListener("mousemove", e => {{
  const t = e.target.closest("[data-tip]");
  if (!t) {{ tip.style.display = "none"; return; }}
  tip.textContent = t.dataset.tip;
  tip.style.display = "block";
  const x = Math.min(e.clientX + 14, innerWidth - tip.offsetWidth - 8);
  tip.style.left = x + "px";
  tip.style.top = (e.clientY + 16) + "px";
}});
</script>
</body></html>
"""


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: visualize.py <db file> <out.html>")
    db_path, out_path = sys.argv[1], sys.argv[2]
    with open(db_path, "rb") as f:
        data = f.read()
    consts = read_constants(db_path)
    with open(out_path, "w") as f:
        f.write(render(db_path, data, consts))
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
