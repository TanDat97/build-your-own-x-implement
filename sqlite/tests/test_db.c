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
#define DB_FILENAME "test.db"

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
      else if (n == 0)
      {
        break; // EOF: child closed stdout
      }
      else if (errno != EAGAIN && errno != EINTR)
      {
        break;
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
  // Ruby checks result[-2]: the response to the last insert, just before the
  // bare prompt that .exit leaves behind.
  expect_line("prints error message when table is full", result, -2,
              "db > Error: Table full.");

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

int main(void)
{
  printf("database\n");

  test_inserts_and_retrieves_a_row();
  test_keeps_data_after_closing_connection();
  test_prints_error_message_when_table_is_full();
  test_allows_inserting_strings_that_are_the_maximum_length();
  test_prints_error_message_if_strings_are_too_long();
  test_prints_an_error_message_if_id_is_negative();

  printf("\n%d test%s, %d failure%s\n", tests_run, tests_run == 1 ? "" : "s",
         tests_failed, tests_failed == 1 ? "" : "s");
  return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
