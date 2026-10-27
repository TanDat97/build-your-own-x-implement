// Black-box tests for the db REPL.
// Each test spawns ./main as a child process, writes commands to its stdin,
// then compares the lines it printed on stdout against expected output.
//
// The REPL is file-backed, so every test starts by deleting DB_FILENAME. Without
// that, rows written by one test would still be there for the next one.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAX_LINES 4096
#define DB_FILENAME "output/test.db" // make test creates output/

typedef struct
{
  char *lines[MAX_LINES];
  int count;
  char *raw;
} Output;

static int tests_run = 0;
static int tests_failed = 0;

// Split on '\n', dropping a trailing empty field (same as Ruby's split("\n")),
// so an output ending in "db > " keeps that last prompt as a line.
// Port of the RSpec suite's `before { `rm -rf test.db` }`: start each test from an
// empty database. Called per test rather than per run_script(), so a test can restart
// the REPL against the same file to check that data persists.
static void delete_db(void)
{
  if (unlink(DB_FILENAME) != 0 && errno != ENOENT)
  {
    perror("unlink " DB_FILENAME);
    exit(EXIT_FAILURE);
  }
}

static void split_lines(Output *output)
{
  char *cursor = output->raw;
  output->count = 0;

  while (*cursor && output->count < MAX_LINES)
  {
    char *newline = strchr(cursor, '\n');
    if (newline == NULL)
    {
      output->lines[output->count++] = cursor;
      break;
    }
    *newline = '\0';
    output->lines[output->count++] = cursor;
    cursor = newline + 1;
  }
}

static Output run_script(const char *commands[], int num_commands)
{
  int to_child[2];
  int from_child[2];

  if (pipe(to_child) < 0 || pipe(from_child) < 0)
  {
    perror("pipe");
    exit(EXIT_FAILURE);
  }

  pid_t pid = fork();
  if (pid < 0)
  {
    perror("fork");
    exit(EXIT_FAILURE);
  }

  if (pid == 0)
  {
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    execl("./main", "./main", DB_FILENAME, (char *)NULL);
    perror("execl ./main");
    _exit(EXIT_FAILURE);
  }

  close(to_child[0]);
  close(from_child[1]);

  // Flatten the script into one buffer so it can be fed to the child a chunk
  // at a time.
  size_t script_len = 0;
  for (int i = 0; i < num_commands; i++)
  {
    script_len += strlen(commands[i]) + 1;
  }
  char *script = malloc(script_len + 1);
  size_t offset = 0;
  for (int i = 0; i < num_commands; i++)
  {
    offset += (size_t)sprintf(script + offset, "%s\n", commands[i]);
  }

  size_t capacity = 65536;
  size_t length = 0;
  char *raw = malloc(capacity);
  size_t written = 0;
  int write_fd = to_child[1];

  fcntl(write_fd, F_SETFL, O_NONBLOCK);

  // Interleave writing with draining stdout. Writing the whole script first
  // would deadlock once it outgrows the 64K pipe buffer (the table-full test
  // sends ~59K of inserts) because the child blocks on its own full stdout.
  while (1)
  {
    struct pollfd fds[2];
    fds[0].fd = from_child[0];
    fds[0].events = POLLIN;
    int nfds = 1;
    int write_idx = -1;
    if (write_fd >= 0)
    {
      write_idx = nfds;
      fds[nfds].fd = write_fd;
      fds[nfds].events = POLLOUT;
      nfds++;
    }

    if (poll(fds, (nfds_t)nfds, -1) < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }
      perror("poll");
      exit(EXIT_FAILURE);
    }

    if (write_idx >= 0 && fds[write_idx].revents)
    {
      ssize_t n = write(write_fd, script + written, script_len - written);
      if (n > 0)
      {
        written += (size_t)n;
      }
      else if (n < 0 && errno != EAGAIN && errno != EINTR)
      {
        written = script_len; // child died or closed stdin; stop writing
      }
      if (written == script_len)
      {
        close(write_fd); // signal EOF, like pipe.close_write
        write_fd = -1;
      }
    }

    if (fds[0].revents)
    {
      if (length + 1 >= capacity)
      {
        capacity *= 2;
        raw = realloc(raw, capacity);
      }
      ssize_t n = read(from_child[0], raw + length, capacity - length - 1);
      if (n > 0)
      {
        length += (size_t)n;
      }
      else if (n == 0 || (errno != EAGAIN && errno != EINTR))
      {
        break; // EOF (child closed stdout), or a read error worth giving up on
      }
    }
  }

  raw[length] = '\0';
  free(script);
  if (write_fd >= 0)
  {
    close(write_fd);
  }
  close(from_child[0]);
  waitpid(pid, NULL, 0);

  Output output = {.raw = raw, .count = 0};
  split_lines(&output);
  return output;
}

static void free_output(Output *output) { free(output->raw); }

static void expect_output(const char *test_name, Output actual,
                          const char *expected[], int expected_count)
{
  tests_run++;

  int ok = (actual.count == expected_count);
  for (int i = 0; ok && i < expected_count; i++)
  {
    ok = (strcmp(actual.lines[i], expected[i]) == 0);
  }

  if (ok)
  {
    printf("  \033[32m✓\033[0m %s\n", test_name);
    return;
  }

  tests_failed++;
  printf("  \033[31m✗\033[0m %s\n", test_name);
  printf("    expected (%d lines):\n", expected_count);
  for (int i = 0; i < expected_count; i++)
  {
    printf("      %d: \"%s\"\n", i, expected[i]);
  }
  printf("    got (%d lines):\n", actual.count);
  for (int i = 0; i < actual.count; i++)
  {
    printf("      %d: \"%s\"\n", i, actual.lines[i]);
  }
}

// Check a single line. A negative index counts back from the end, like Ruby's
// result[-2].
static void expect_line(const char *test_name, Output actual, int index,
                        const char *expected)
{
  tests_run++;

  int i = index < 0 ? actual.count + index : index;
  const char *got =
      (i >= 0 && i < actual.count) ? actual.lines[i] : "<index out of range>";

  if (strcmp(got, expected) == 0)
  {
    printf("  \033[32m✓\033[0m %s\n", test_name);
    return;
  }

  tests_failed++;
  printf("  \033[31m✗\033[0m %s\n", test_name);
  printf("    line %d expected: \"%s\"\n", index, expected);
  printf("    line %d got:      \"%s\"\n", index, got);
}

// Return a view of `output` without its first `n` lines, like Ruby's
// result[n...result.length]. It shares the original's buffer, so free only the
// original.
static Output drop_lines(Output output, int n)
{
  Output tail = {.raw = output.raw, .count = 0};
  for (int i = n; i < output.count; i++)
  {
    tail.lines[tail.count++] = output.lines[i];
  }
  return tail;
}

static char *repeat_char(char c, int n)
{
  char *s = malloc((size_t)n + 1);
  memset(s, c, (size_t)n);
  s[n] = '\0';
  return s;
}

static void test_inserts_and_retrieves_a_row(void)
{
  delete_db();

  const char *commands[] = {
      "insert 1 user1 person1@example.com",
      "select",
      ".exit",
  };
  const char *expected[] = {
      "db > Executed.",
      "db > (1, user1, person1@example.com)",
      "Executed.",
      "db > ",
  };

  Output result = run_script(commands, 3);
  expect_output("inserts and retrieves a row", result, expected, 4);
  free_output(&result);
}

static void test_keeps_data_after_closing_connection(void)
{
  delete_db();

  const char *insert_commands[] = {
      "insert 1 user1 person1@example.com",
      ".exit",
  };
  const char *insert_expected[] = {
      "db > Executed.",
      "db > ",
  };

  Output result1 = run_script(insert_commands, 2);
  expect_output("keeps data after closing connection (insert)", result1,
                insert_expected, 2);
  free_output(&result1);

  // Second run: a fresh REPL against the same file, so the row can only come
  // from what db_close() flushed to disk.
  const char *select_commands[] = {
      "select",
      ".exit",
  };
  const char *select_expected[] = {
      "db > (1, user1, person1@example.com)",
      "Executed.",
      "db > ",
  };

  Output result2 = run_script(select_commands, 2);
  expect_output("keeps data after closing connection (select)", result2,
                select_expected, 3);
  free_output(&result2);
}

static void test_prints_error_message_when_table_is_full(void)
{
  delete_db();

  const int num_inserts = 1401;
  const char **commands = malloc(((size_t)num_inserts + 1) * sizeof(char *));

  for (int i = 1; i <= num_inserts; i++)
  {
    char *command = malloc(64);
    snprintf(command, 64, "insert %d user%d person%d@example.com", i, i, i);
    commands[i - 1] = command;
  }
  commands[num_inserts] = ".exit";

  Output result = run_script(commands, num_inserts + 1);
  // Ruby checks result.last(2). Inserts now find their leaf through the root, but
  // once the right leaf fills up it has to split under an existing parent, which
  // is not implemented yet, so the REPL exits there instead of reaching a
  // table-full error.
  expect_line("prints error message when table is full (last insert)", result,
              -2, "db > Executed.");
  expect_line("prints error message when table is full (exit)", result, -1,
              "db > Need to implement updating parent after split");

  free_output(&result);
  for (int i = 0; i < num_inserts; i++)
  {
    free((void *)commands[i]);
  }
  free(commands);
}

static void test_allows_inserting_strings_that_are_the_maximum_length(void)
{
  delete_db();

  char *long_username = repeat_char('a', 32);
  char *long_email = repeat_char('a', 255);

  char insert[512];
  snprintf(insert, sizeof(insert), "insert 1 %s %s", long_username, long_email);
  char row[512];
  snprintf(row, sizeof(row), "db > (1, %s, %s)", long_username, long_email);

  const char *commands[] = {insert, "select", ".exit"};
  const char *expected[] = {
      "db > Executed.",
      row,
      "Executed.",
      "db > ",
  };

  Output result = run_script(commands, 3);
  expect_output("allows inserting strings that are the maximum length", result,
                expected, 4);

  free_output(&result);
  free(long_username);
  free(long_email);
}

static void test_prints_error_message_if_strings_are_too_long(void)
{
  delete_db();

  char *long_username = repeat_char('a', 33);
  char *long_email = repeat_char('a', 256);

  char insert[512];
  snprintf(insert, sizeof(insert), "insert 1 %s %s", long_username, long_email);

  const char *commands[] = {insert, "select", ".exit"};
  const char *expected[] = {
      "db > String is too long.",
      "db > Executed.",
      "db > ",
  };

  Output result = run_script(commands, 3);
  expect_output("prints error message if strings are too long", result,
                expected, 3);

  free_output(&result);
  free(long_username);
  free(long_email);
}

static void test_prints_an_error_message_if_id_is_negative(void)
{
  delete_db();

  const char *commands[] = {
      "insert -1 cstack foo@bar.com",
      "select",
      ".exit",
  };
  const char *expected[] = {
      "db > ID must be positive.",
      "db > Executed.",
      "db > ",
  };

  Output result = run_script(commands, 3);
  expect_output("prints an error message if id is negative", result, expected,
                3);
  free_output(&result);
}

static void test_prints_constants(void)
{
  delete_db();

  const char *commands[] = {
      ".constants",
      ".exit",
  };
  const char *expected[] = {
      "db > Constants:",
      "ROW_SIZE: 293",
      "COMMON_NODE_HEADER_SIZE: 6",
      "LEAF_NODE_HEADER_SIZE: 14",
      "LEAF_NODE_CELL_SIZE: 297",
      "LEAF_NODE_SPACE_FOR_CELLS: 4082",
      "LEAF_NODE_MAX_CELLS: 13",
      "db > ",
  };

  Output result = run_script(commands, 2);
  expect_output("prints constants", result, expected, 8);
  free_output(&result);
}

static void test_allows_printing_out_the_structure_of_a_one_node_btree(void)
{
  delete_db();

  const char *commands[] = {
      "insert 3 user3 person3@example.com",
      "insert 1 user1 person1@example.com",
      "insert 2 user2 person2@example.com",
      ".btree",
      ".exit",
  };
  const char *expected[] = {
      "db > Executed.",
      "db > Executed.",
      "db > Executed.",
      "db > Tree:",
      "- leaf (size 3)",
      "  - 1",
      "  - 2",
      "  - 3",
      "db > ",
  };

  Output result = run_script(commands, 5);
  expect_output("allows printing out the structure of a one-node btree", result,
                expected, 9);
  free_output(&result);
}

static void test_allows_printing_out_the_structure_of_a_3_leaf_node_btree(void)
{
  delete_db();

  const int num_inserts = 14;
  const char *commands[17];
  char inserts[15][64];

  for (int i = 1; i <= num_inserts; i++)
  {
    snprintf(inserts[i - 1], 64, "insert %d user%d person%d@example.com", i, i, i);
    commands[i - 1] = inserts[i - 1];
  }
  commands[14] = ".btree";
  snprintf(inserts[14], 64, "insert 15 user15 person15@example.com");
  commands[15] = inserts[14];
  commands[16] = ".exit";

  const char *expected[] = {
      "db > Tree:",
      "- internal (size 1)",
      "  - leaf (size 7)",
      "    - 1",
      "    - 2",
      "    - 3",
      "    - 4",
      "    - 5",
      "    - 6",
      "    - 7",
      "  - key 7",
      "  - leaf (size 7)",
      "    - 8",
      "    - 9",
      "    - 10",
      "    - 11",
      "    - 12",
      "    - 13",
      "    - 14",
      "db > Executed.",
      "db > ",
  };

  Output result = run_script(commands, 17);
  // Ruby checks result[14...result.length]: skip the 14 "Executed." lines.
  expect_output("allows printing out the structure of a 3-leaf-node btree",
                drop_lines(result, num_inserts), expected, 21);
  free_output(&result);
}

static void test_prints_all_rows_in_a_multi_level_tree(void)
{
  delete_db();

  const int num_inserts = 15;
  const char *commands[17];
  char inserts[15][64];

  for (int i = 1; i <= num_inserts; i++)
  {
    snprintf(inserts[i - 1], 64, "insert %d user%d person%d@example.com", i, i, i);
    commands[i - 1] = inserts[i - 1];
  }
  commands[15] = "select";
  commands[16] = ".exit";

  const char *expected[] = {
      "db > (1, user1, person1@example.com)",
      "(2, user2, person2@example.com)",
      "(3, user3, person3@example.com)",
      "(4, user4, person4@example.com)",
      "(5, user5, person5@example.com)",
      "(6, user6, person6@example.com)",
      "(7, user7, person7@example.com)",
      "(8, user8, person8@example.com)",
      "(9, user9, person9@example.com)",
      "(10, user10, person10@example.com)",
      "(11, user11, person11@example.com)",
      "(12, user12, person12@example.com)",
      "(13, user13, person13@example.com)",
      "(14, user14, person14@example.com)",
      "(15, user15, person15@example.com)",
      "Executed.",
      "db > ",
  };

  Output result = run_script(commands, 17);
  // Ruby checks result[15...result.length]: skip the 15 "Executed." lines.
  expect_output("prints all rows in a multi-level tree",
                drop_lines(result, num_inserts), expected, 17);
  free_output(&result);
}

int main(void)
{
  printf("database\n");

  test_inserts_and_retrieves_a_row();
  test_keeps_data_after_closing_connection();
  test_prints_error_message_when_table_is_full();
  test_allows_inserting_strings_that_are_the_maximum_length();
  test_prints_error_message_if_strings_are_too_long();
  test_prints_an_error_message_if_id_is_negative();
  test_prints_constants();
  test_allows_printing_out_the_structure_of_a_one_node_btree();
  test_allows_printing_out_the_structure_of_a_3_leaf_node_btree();
  test_prints_all_rows_in_a_multi_level_tree();

  printf("\n%d test%s, %d failure%s\n", tests_run, tests_run == 1 ? "" : "s",
         tests_failed, tests_failed == 1 ? "" : "s");
  return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
