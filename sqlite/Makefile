CC     = gcc
CFLAGS = -Wall -Wextra -g

main: main.c
	$(CC) $(CFLAGS) -o main main.c

tests/test_db: tests/test_db.c
	$(CC) $(CFLAGS) -o tests/test_db tests/test_db.c

# ./main must sit in the working directory, since the tests exec "./main"
.PHONY: test clean
test: main tests/test_db
	./tests/test_db

clean:
	rm -f main tests/test_db test.db
