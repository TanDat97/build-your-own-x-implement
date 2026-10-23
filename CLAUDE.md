# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository layout

A collection of "build your own X" implementations, one directory per project. Each
project is self-contained: its own build system, tests and toolchain. There is no
top-level build. Work from inside the project directory.

Current projects:

- `sqlite/` — a SQLite-style database REPL in C, following cstack's
  *Let's Build a Simple Database* tutorial. Built incrementally, part by part.

`README.md` is the user-facing overview and lists every tool in a table. When a new
tool folder is added, add its row there and a section here.

Ignore rules are split the same way. The root `.gitignore` holds what is true for
every project — generic build artifacts, debug/crash files, editor and OS noise,
and a `.vscode/*` block that whitelists the shared configs (`launch.json`,
`tasks.json`, `settings.json`, …). A project `.gitignore` only adds what the root
cannot know: its own build targets and data files. Put a new rule in the narrowest
file that covers it, and don't restate a root rule in a project file.

## sqlite/

### Commands

All commands run from `sqlite/`:

```bash
make            # build the REPL -> ./main
make test       # build ./main and ./tests/test_db.db, then run the suite
make clean      # also removes output/
./main mydb.db  # run the REPL directly
make viz        # render output/trace.db to output/viz.html and open it
```

Every generated file lives in `output/` (created on demand, git-ignored as a whole):
`test.db` from the tests, and `trace.db`, `trace.html`, `viz.html` from the two HTML
tools. `viz.html` and `trace.html` describe the same file, `output/trace.db`:
`make trace` writes it and renders both pages, and `make viz` only re-renders
`viz.html` from it (running the trace script first if `trace.db` is missing).

`make viz` runs `tools/visualize.py`, which reads the layout sizes from
`./main`'s `.constants` (run against a temp copy, so the db is not rewritten),
prints the tree from page 0 the way `.btree` does, and draws each page as a leaf
or internal node (by its node-type byte): byte-layout bar, cell table,
colour-coded hex dump. `.constants` only prints leaf sizes, so the Row field
widths and the internal-node layout are mirrored at the top of the script; the Row
widths are checked against `ROW_SIZE`. Update them there if `Row` or the internal
node layout changes.

```bash
make trace              # default run: fill the root leaf, then split it -> output/trace.html
make trace IN=cmds.txt  # your own commands; a line of `---` restarts ./main
```

`make trace` runs `./main output/trace.db` under gdb (`tools/trace_gdb.py`), breaking on
every function in `main.c` (found by regex, so new functions are picked up
automatically). It records arguments, return values and argument state after the
call, maps raw pointers to "page N + byte K", and snapshots the page cache and the
file before each prompt, after each `pager_flush` and at exit. `tools/trace.py`
renders that per command, plus a B-tree graph per step and a per-session
filmstrip of the tree, with each function's description taken from the comment
above it — another reason to keep those comments current. The layer grouping in
`LAYERS` at the top of `trace.py` is manual; unlisted functions show as "Other".
The tree walk starts at page 0 and draws internal nodes: `trace_gdb.py` parses
them from the tutorial's `INTERNAL_NODE_*` constants and the node-type byte. The
insert cursor is read from `table_find()`'s return value. gdb exits 0 even when
`trace_gdb.py` raises (the exception just halts `./main` mid-run), so `trace.py`
scans gdb's stderr for `Python Exception` and fails loudly instead of rendering a
truncated trace.

`main` requires a database filename; with no argument it prints
`"Must supply a database filename."` and exits.

`CFLAGS` is `-Wall -Wextra -g` plus `-Werror=implicit-function-declaration` and
`-Werror=int-conversion`: in C those two are warnings by default, and both are
easy to hit while following the tutorial (calling a function before its
definition appears, or returning a pointer where an integer is expected). Making
them hard errors keeps a half-wired refactor from producing a binary that builds
and then misbehaves at runtime.

`make test` must be run from `sqlite/` — `tests/test_db.db` execs `"./main"` by
relative path, so the binary has to sit in the working directory. The tests use
`output/test.db`; the `test` target creates `output/` first, since `main` opens
the file with `O_CREAT` but cannot create the directory.

Note that compiling straight to `./main` by hand gives the binary a newer mtime
than `main.c`, so a following `make main` is a no-op. `rm -f main` first.

There is no per-test filter. To run a single case, comment out the other
`test_*()` calls in `main()` of `tests/test_db.c` and rebuild, or add a
temporary `argv` check.

### Architecture

`main.c` is a single-file REPL split into the tutorial's layers, top to bottom:

1. **Input** — `InputBuffer` + `read_input()` wrap `getline()`.
2. **Front end** — `do_meta_command()` handles `.`-prefixed commands (`.exit`,
   `.btree`, `.constants`); `prepare_statement()` / `prepare_insert()` parse SQL
   into a `Statement` (a `StatementType` plus an inline `Row`). `.btree` prints the
   whole tree through `print_tree()`, which recurses from page 0 and uses
   `indent()` for nesting.
3. **Virtual machine** — `execute_statement()` dispatches to `execute_insert` /
   `execute_select` against a `Table`.
4. **Cursor** — a `Cursor` (a `Table *`, a `page_num`, a `cell_num` and an
   `end_of_table` flag) is the only way the VM addresses rows. `table_find(key)`
   descends from the root — `internal_node_find()` binary-searches an internal
   node's keys for the child to follow, `leaf_node_find()` binary-searches a leaf
   — and returns a cursor at the key, or where it would go to keep the leaf sorted.
   `execute_insert` uses that cursor (and reports `EXECUTE_DUPLICATE_KEY` if the
   key is already there); `execute_select` walks a `table_start()` cursor with
   `cursor_advance()` until `end_of_table`. Both `free()` it: cursors are
   heap-allocated and owned by the caller. `cursor_value()` turns a position into
   the address of that cell's row.
5. **B-tree nodes** — every page is one node. A 6-byte common header holds the
   node type, an is-root flag and a parent pointer. A **leaf** adds `num_cells` and
   packs up to `LEAF_NODE_MAX_CELLS` (13) cells of `key` + serialized `Row`. An
   **internal** node adds `num_keys` and a right-child page number, then
   (child page, key) cells, where each key is the max key of the child to its
   left. The `leaf_node_*` / `internal_node_*` accessors are pointer arithmetic
   into a page; `internal_node_child(n)` returns the header's right child when
   `n == num_keys`. `serialize_row` / `deserialize_row` move a `Row` to and from
   the on-page layout via the `*_OFFSET` constants, so page bytes are the on-disk
   format, not the C struct. `serialize_row` uses `strncpy` (not `memcpy`) for the
   two string columns so the bytes after each terminator are zero-filled rather
   than leftover stack garbage, keeping written pages deterministic. `id` still
   goes through `memcpy`; it is not a string.
6. **Insert and split** — `leaf_node_insert()` shifts later cells right and writes
   the new one. When the leaf is full it calls `leaf_node_split_and_insert()`,
   which moves the upper `LEAF_NODE_RIGHT_SPLIT_COUNT` (7) cells into a new leaf
   on `get_unused_page_num()` (always the end of the file, no free-page reuse) and
   keeps `LEAF_NODE_LEFT_SPLIT_COUNT` (7) in place. If the split leaf was the root,
   `create_new_root()` copies it to another new page as the left child and
   rewrites page 0 as an internal node over the two leaves, so the root always
   stays on page 0. Splitting a non-root leaf (updating its parent) is not
   implemented yet and exits with `Need to implement updating parent after split`.
7. **Pager** — `pager_open()` / `get_page()` own the file descriptor and a
   `TABLE_MAX_PAGES` array of lazily-allocated 4K page caches; a page is read
   from disk only on first touch (cache miss), and touching a page past the end
   grows `num_pages`. The file must be a whole number of pages.
8. **Persistence** — `db_open()` builds the pager and a `Table` (a `Pager` plus
   `root_page_num`, always 0); a new file gets page 0 initialized as an empty root
   leaf. `.exit` routes through `do_meta_command(input_buffer, table)` to
   `db_close()`, which flushes every cached page in full with `pager_flush()`,
   closes the fd and frees everything. Nothing is written before `.exit`, so a run
   that dies on an `exit()` (such as the split-under-a-parent case) leaves the file
   as it was.

Every layer returns a result enum (`MetaCommandResult`, `PrepareResult`,
`ExecuteResult`) rather than printing; the `switch` statements in `main()` are
the only place user-facing error strings live. Keep new failure modes as enum
variants handled there — the tests assert on exact output lines. The exceptions
are the tutorial's unimplemented-path stubs ("Need to implement …"), which
`printf` and `exit()` in place, and the tests assert on those too.
`EXECUTE_TABLE_FULL` is still defined and handled but no longer returned.

Fixed limits that the tests depend on: `COLUMN_USERNAME_SIZE` 32,
`COLUMN_EMAIL_SIZE` 255 (so `ROW_SIZE` 293, `LEAF_NODE_CELL_SIZE` 297),
`PAGE_SIZE` 4096, `LEAF_NODE_MAX_CELLS` 13, `TABLE_MAX_PAGES` 100. The
`.constants` test checks those layout sizes exactly.

### Tests

`tests/test_db.c` is a standalone black-box harness (a C port of the tutorial's
RSpec suite) with no framework. `run_script()` forks `./main`, pipes a script to
its stdin and captures stdout, then `expect_output()` compares the captured
lines against expected ones exactly — including the `db > ` prompt fragments,
which is why expected lines look like `"db > Executed."` and the last line is a
bare `"db > "` (or the stub message, when the run ends on an `exit()`).

Writes and reads are interleaved through `poll()` on purpose: writing the whole
script first deadlocks once it exceeds the 64K pipe buffer (the table-full test
sends ~59K). Preserve that when touching the harness.

`expect_line()` accepts negative indices (counting from the end), matching the
tutorial's `result[-2]`. `drop_lines(output, n)` returns a view without the first
`n` lines, for the tutorial's `result[14...result.length]`; it shares the
original's buffer, so only the original is freed.

Because the REPL is file-backed, each `test_*()` starts with `delete_db()` (the
port of the RSpec suite's `before` hook running `rm -rf test.db`) and `run_script()` execs
`./main output/test.db`. `delete_db()` is deliberately called per test rather than inside
`run_script()`, so a test can run the REPL twice against the same file — which is
exactly what `test_keeps_data_after_closing_connection()` does to prove rows
survive a restart. A single RSpec example that calls `run_script` twice, or checks
two separate lines, becomes two assertions here, so that one test reports as two.

The table-full test keeps the tutorial's 1401 inserts even though the run now
stops much earlier: it is the test that pushes the harness past the 64K pipe
buffer, and later tutorial parts grow the tree until it hits `TABLE_MAX_PAGES`.
It checks the last two lines — `db > Executed.` and, for now,
`db > Need to implement updating parent after split` — and that expected last
line changes as each missing piece lands.

### Current state

The tutorial is through *Splitting a Leaf Node* and recursive search of internal
nodes: rows live in a B-tree, a full root leaf splits into an internal root over
two leaves, and inserts and lookups descend through internal nodes. The suite is
green: 11 assertions from 8 `test_*()` functions, 0 failures (`make test`).

Every function in `main.c` carries a comment describing what it does; keep that up
when adding new ones (`trace.py` shows them, and only reads `//` lines directly
above the function).

Known gaps, left as the tutorial leaves them at this point:

- `execute_select` scans only from `table_start()`, which reads the root as a
  leaf, so `select` is wrong once the root is internal.
- `execute_insert`'s duplicate-key check reads the root page as a leaf rather than
  the leaf `table_find()` landed on.
- In `leaf_node_split_and_insert`, the new row is serialized at the start of its
  cell instead of at `leaf_node_value()`, and its key is not written; it only
  looks right because `id` equals the key.
- Splitting a non-root leaf exits (see layer 6), which caps the table at three
  nodes for now.

The next tutorial step is updating the parent after a split, which removes that
`exit()` and lets the tree grow past two leaves.
