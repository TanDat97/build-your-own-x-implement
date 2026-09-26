CC     = gcc
CFLAGS = -Wall -Wextra -g -Werror=implicit-function-declaration -Werror=int-conversion

# Every generated .db and .html file goes here.
OUT = output

main: main.c
	$(CC) $(CFLAGS) -o main main.c

tests/test_db: tests/test_db.c
	$(CC) $(CFLAGS) -o tests/test_db tests/test_db.c

# ./main must sit in the working directory, since the tests exec "./main"
.PHONY: test clean viz trace
test: main tests/test_db
	@mkdir -p $(OUT)
	./tests/test_db

# Both HTML views read the same database, $(OUT)/trace.db, which `make trace` writes.
# `make trace` runs the REPL under gdb, records every call into main.c, renders
# trace.html and then viz.html from the resulting trace.db.
# `make trace IN=cmds.txt` uses your own commands (a line of `---` restarts ./main).
trace: main
	python3 tools/trace.py $(IN)
	python3 tools/visualize.py $(OUT)/trace.db $(OUT)/viz.html
	@xdg-open $(OUT)/trace.html > /dev/null 2>&1 || echo "open $(OUT)/trace.html in a browser"

# Render trace.db as viz.html and open it, running `make trace`'s script first if
# trace.db does not exist yet.
viz: main
	@[ -f $(OUT)/trace.db ] || python3 tools/trace.py $(IN)
	python3 tools/visualize.py $(OUT)/trace.db $(OUT)/viz.html
	@xdg-open $(OUT)/viz.html > /dev/null 2>&1 || echo "open $(OUT)/viz.html in a browser"

clean:
	rm -f main tests/test_db
	rm -rf $(OUT)
