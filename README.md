# build-your-own-x-implement

Common developer tools, rebuilt from scratch to understand how they work.

Each top-level folder is one standalone tool with its own source, build system and
tests. There is no shared library and no top-level build — pick a folder, `cd` into
it, and build it on its own.

## Tools

| Folder | Tool | Language | Status |
| --- | --- | --- | --- |
| [`sqlite/`](sqlite/) | A simple database with a SQL-ish REPL and file-backed storage | C | In progress |

More tools will be added as separate folders.

## sqlite

A miniature database, built along the lines of cstack's
[*Let's Build a Simple Database*](https://cstack.github.io/db_tutorial/). It's a
REPL that accepts `insert` and `select` statements plus `.`-prefixed meta commands,
stores rows in 4KB pages, and persists them to a file.

```bash
cd sqlite
make            # build ./main
make test       # build and run the test suite
./main mydb.db  # start the REPL against a database file
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

The layers it is built from — input buffer, statement parser, virtual machine, and
a pager over a fixed-size page cache — mirror the structure of real SQLite, at a
much smaller scale. Rows live in 4KB pages that are read from the file on first
touch and flushed back when the database is closed.

Tests live in `sqlite/tests/test_db.c` and drive the compiled REPL as a black box:
they pipe a script into its stdin and compare stdout line by line. It is a C port
of the tutorial's RSpec suite, with no test framework — `make test` runs all 7.

## Requirements

A C compiler (`gcc`) and `make`. Tools added later may bring their own toolchains;
each folder documents what it needs.
