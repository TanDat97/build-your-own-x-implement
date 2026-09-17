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

## sqlite/

### Commands

All commands run from `sqlite/`:

```bash
make            # build the REPL -> ./main
make test       # build ./main and ./tests/test_db, then run the suite
make clean
./main mydb.db  # run the REPL directly
```

`make test` must be run from `sqlite/` — `tests/test_db` execs `"./main"` by
relative path, so the binary has to sit in the working directory.

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
4. **Storage** — `Table` holds a row count and a `Pager`. `row_slot()` maps a row
   number to a byte offset: `page_num = row_num / ROWS_PER_PAGE`, then
   `row_offset * ROW_SIZE` within the page. `serialize_row` / `deserialize_row`
   memcpy the `Row` struct into a compact on-page layout via the `*_OFFSET`
   constants, so page bytes are the on-disk format, not the C struct.
5. **Pager** — `pager_open()` / `get_page()` own the file descriptor and a
   `TABLE_MAX_PAGES` array of lazily-allocated 4K page caches; a page is read
   from disk only on first touch (cache miss).

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

### Current state

`main.c` has an in-progress, non-compiling refactor that adds file-backed
persistence (`Pager`, `pager_open`, `db_open`). Leftovers from the pre-pager
version still reference the removed `table->pages`: `row_slot()` declares `page`
twice, `free_table()` iterates `table->pages`, and `main()` calls the deleted
`new_table()` instead of `db_open(argv[1])`. There is no `db_close()` flushing
pages back to disk yet. Finish this before adding new features; the test suite
cannot run until it builds.
