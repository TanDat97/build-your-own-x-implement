# build-your-own-x-implement

Common developer tools, rebuilt from scratch to understand how they work.

Each top-level folder is one standalone tool with its own source, build system and
tests. There is no shared library and no top-level build — pick a folder, `cd` into
it, and build it on its own.

## Tools

| Folder | Tool | Language | Status |
| --- | --- | --- | --- |
| [`sqlite/`](sqlite/) | A simple database with a SQL-ish REPL and B-tree storage in a file | C | In progress |

More tools will be added as separate folders.

## sqlite

A miniature database, built along the lines of cstack's
[*Let's Build a Simple Database*](https://cstack.github.io/db_tutorial/). It's a
REPL that accepts `insert` and `select` statements plus `.`-prefixed meta commands
(`.exit`, `.btree`, `.constants`), stores rows in a B-tree of 4KB pages, and
persists them to a file.

```bash
cd sqlite
make            # build ./main
make test       # build and run the test suite
./main mydb.db  # start the REPL against a database file
make trace      # trace a run under gdb -> output/trace.html (needs gdb, python3)
make viz        # draw the database file's pages -> output/viz.html
```

The filename is required. Rows are written back to it on `.exit`, so reopening the
same file brings them back:

```
$ ./main mydb.db
db > insert 1 user1 person1@example.com
Executed.
db > .exit

$ ./main mydb.db
db > select
(1, user1, person1@example.com)
Executed.
db > .exit
```

`.btree` prints the tree. After 14 inserts the root leaf, which holds 13 rows, has
split into an internal node over two leaves:

```
db > .btree
Tree:
- internal (size 1)
  - leaf (size 7)
    - 1
    ...
    - 7
  - key 7
  - leaf (size 7)
    - 8
    ...
    - 14
```

The layers it is built from — input buffer, statement parser, virtual machine,
cursor, B-tree nodes and a pager over a fixed-size page cache — mirror the
structure of real SQLite, at a much smaller scale. Every page is one B-tree node.
Leaves hold rows sorted by id, internal nodes hold keys and child page numbers, and
inserts binary-search down from the root to find their leaf. Pages are read from
the file on first touch and flushed back when the database is closed.

Tests live in `sqlite/tests/test_db.c` and drive the compiled REPL as a black box:
they pipe a script into its stdin and compare stdout line by line. It is a C port
of the tutorial's RSpec suite, with no test framework — `make test` runs all 11
checks.

Two HTML tools in `sqlite/tools/` show what the code does. `make trace` runs the
REPL under gdb and records every call, with its arguments and return value, plus
the page cache and the file after each command. `make viz` draws each page of the
database file byte by byte. Both write to `sqlite/output/`; `make trace IN=cmds.txt`
traces your own commands.

Progress so far: the REPL and statement parser, file-backed persistence, the
cursor abstraction, the B-tree leaf format, binary search with duplicate-key
errors, splitting a full root leaf, and searching through internal nodes. Next is
updating the parent after a split: until then, a leaf that fills up under an
internal root ends the program with `Need to implement updating parent after
split`. `select` also still reads only the root, so it is wrong once the root has
split.

## Requirements

A C compiler (`gcc`) and `make`. For `sqlite/`'s `make trace` and `make viz`, also
`gdb` (with Python support) and `python3`. Tools added later may bring their own
toolchains; each folder documents what it needs.

Build output and database files are ignored by git: the root `.gitignore` covers
what every project shares (object files, debug and crash dumps, editor and OS
noise), and each tool folder adds its own targets — for `sqlite/` that is `main`,
`tests/test_db.db`, `output/` and `*.db`.
