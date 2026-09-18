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
make test       # build ./main and ./tests/test_db, then run the suite
make clean      # also removes test.db
./main mydb.db  # run the REPL directly
```

`main` requires a database filename; with no argument it prints
`"Must supply a database filename."` and exits.

`CFLAGS` is `-Wall -Wextra -g` plus `-Werror=implicit-function-declaration` and
`-Werror=int-conversion`: in C those two are warnings by default, and both are
easy to hit while following the tutorial (calling a function before its
definition appears, or returning a pointer where an integer is expected). Making
them hard errors keeps a half-wired refactor from producing a binary that builds
and then misbehaves at runtime.

`make test` must be run from `sqlite/` — `tests/test_db` execs `"./main"` by
relative path, so the binary has to sit in the working directory. The tests use
`test.db` in that same directory.

Note that compiling straight to `./main` by hand gives the binary a newer mtime
than `main.c`, so a following `make main` is a no-op. `rm -f main` first.

There is no per-test filter. To run a single case, comment out the other
`test_*()` calls in `main()` of `tests/test_db.c` and rebuild, or add a
temporary `argv` check.

### Architecture

`main.c` is a single-file REPL split into the tutorial's layers, top to bottom:

1. **Input** — `InputBuffer` + `read_input()` wrap `getline()`.
2. **Front end** — `do_meta_command()` handles `.`-prefixed commands;
   `prepare_statement()` / `prepare_insert()` parse SQL into a `Statement`
   (a `StatementType` plus an inline `Row`).
3. **Virtual machine** — `execute_statement()` dispatches to `execute_insert` /
   `execute_select` against a `Table`.
4. **Cursor** — a `Cursor` (a `Table *`, a `row_num` and an `end_of_table` flag)
   is the only way the VM addresses rows. `table_start()` / `table_end()` make one
   pointing at the first row or one past the last; `cursor_advance()` steps it and
   raises `end_of_table` on reaching `num_rows`. `execute_insert` writes through a
   `table_end()` cursor, `execute_select` walks a `table_start()` one until
   `end_of_table`, and both `free()` it. Cursors are heap-allocated and owned by
   the caller. This layer exists to give the B-tree somewhere to land: once rows
   live in a tree, only the cursor internals change and the VM stays as it is.
5. **Storage** — `Table` holds a row count and a `Pager`. `cursor_value()` maps the
   cursor's row number to a byte address: `page_num = row_num / ROWS_PER_PAGE`, then
   `row_offset * ROW_SIZE` within the page. (This is the tutorial's `row_slot()`,
   renamed once the cursor took over addressing.) `serialize_row` / `deserialize_row`
   move the `Row` struct to and from a compact on-page layout via the `*_OFFSET`
   constants, so page bytes are the on-disk format, not the C struct.
   `serialize_row` uses `strncpy` (not `memcpy`) for the two string columns so the
   bytes after each terminator are zero-filled rather than leftover stack garbage —
   that keeps written pages deterministic instead of leaking uninitialised memory
   into the file. `id` still goes through `memcpy`; it is not a string.
6. **Pager** — `pager_open()` / `get_page()` own the file descriptor and a
   `TABLE_MAX_PAGES` array of lazily-allocated 4K page caches; a page is read
   from disk only on first touch (cache miss).
7. **Persistence** — `db_open()` builds the pager and derives `num_rows` from the
   file length. `.exit` routes through `do_meta_command(input_buffer, table)` to
   `db_close()`, which flushes each cached page with `pager_flush()`, closes the
   fd and frees everything. `pager_flush` takes a byte count rather than always
   writing `PAGE_SIZE`, because rows are packed tightly and the last page is
   usually partial. There is no `free_table()` any more — `db_close()` replaced it.

Every layer returns a result enum (`MetaCommandResult`, `PrepareResult`,
`ExecuteResult`) rather than printing or exiting; the `switch` statements in
`main()` are the only place user-facing error strings live. Keep new failure
modes as enum variants handled there — the tests assert on exact output lines.

Fixed limits that the tests depend on: `COLUMN_USERNAME_SIZE` 32,
`COLUMN_EMAIL_SIZE` 255, `PAGE_SIZE` 4096, `TABLE_MAX_PAGES` 100, giving
1400 max rows (the table-full test inserts 1401).

### Tests

`tests/test_db.c` is a standalone black-box harness (a C port of the tutorial's
RSpec suite) with no framework. `run_script()` forks `./main`, pipes a script to
its stdin and captures stdout, then `expect_output()` compares the captured
lines against expected ones exactly — including the `db > ` prompt fragments,
which is why expected lines look like `"db > Executed."` and the last line is a
bare `"db > "`.

Writes and reads are interleaved through `poll()` on purpose: writing the whole
script first deadlocks once it exceeds the 64K pipe buffer (the table-full test
sends ~59K). Preserve that when touching the harness.

`expect_line()` accepts negative indices (counting from the end), matching the
tutorial's `result[-2]`.

Because the REPL is file-backed, each `test_*()` starts with `delete_db()` (the
port of the RSpec suite's `before` hook running `rm -rf test.db`) and `run_script()` execs
`./main test.db`. `delete_db()` is deliberately called per test rather than inside
`run_script()`, so a test can run the REPL twice against the same file — which is
exactly what `test_keeps_data_after_closing_connection()` does to prove rows
survive a restart. A single RSpec example that calls `run_script` twice becomes two
`expect_output()` assertions here, so that one test reports as two.

### Current state

File-backed persistence and the cursor abstraction are both done, and the suite is
green: 7 tests, 0 failures (`make test`). The cursor change is a pure refactor —
it added no behaviour and no test, which is why the count did not move.

Every function in `main.c` carries a comment describing what it does; keep that up
when adding new ones.

The next step in the tutorial is replacing the append-only row array with a
B-tree, which is what the "should not be needed after we switch to a B-tree"
comment in `db_close()` refers to. Rows are addressed only through the cursor now,
so that work is mostly confined to the cursor and pager layers.
