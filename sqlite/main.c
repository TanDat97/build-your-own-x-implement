#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

typedef struct
{
  char *buffer;
  size_t buffer_length;
  ssize_t input_length;
} InputBuffer;

typedef enum
{
  EXECUTE_SUCCESS,
  EXECUTE_TABLE_FULL
} ExecuteResult;

typedef enum
{
  META_COMMAND_SUCCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum
{
  PREPARE_SUCCESS,
  PREPARE_NEGATIVE_ID,
  PREPARE_STRING_TOO_LONG,
  PREPARE_UNRECOGNIZED_STATEMENT,
  PREPARE_SYNTAX_ERROR
} PrepareResult;

typedef enum
{
  STATEMENT_INSERT,
  STATEMENT_SELECT
} StatementType;

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255
typedef struct
{
  uint32_t id;
  char username[COLUMN_USERNAME_SIZE + 1];
  char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef struct
{
  StatementType type;
  Row row_to_insert;
} Statement;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct *)0)->Attribute)

const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

const uint32_t PAGE_SIZE = 4096;
#define TABLE_MAX_PAGES 100

typedef struct
{
  int file_descriptor;
  uint32_t file_length;
  uint32_t num_pages;
  void *pages[TABLE_MAX_PAGES];
} Pager;

typedef struct
{
  Pager *pager;
  uint32_t root_page_num;
} Table;

typedef struct
{
  Table *table;
  uint32_t page_num;
  uint32_t cell_num;
  bool end_of_table; // Indicates a position one past the last element
} Cursor;

typedef enum
{
  NODE_INTERNAL,
  NODE_LEAF
} NodeType;
/*
 * Common Node Header Layout
 */
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
const uint8_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;
/*
 * Leaf Node Header Layout
 */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE;

/*
 * Leaf Node Body Layout
 */
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET = 0;
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;
const uint32_t LEAF_NODE_VALUE_OFFSET = LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE;
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;

// Return a pointer to the leaf node's cell count, stored in its header right after the
// common node header.
uint32_t *leaf_node_num_cells(void *node)
{
  return node + LEAF_NODE_NUM_CELLS_OFFSET;
}

// Return a pointer to cell `cell_num` of a leaf node: cells are packed back to back
// after the leaf header, each a key followed by a serialized row.
void *leaf_node_cell(void *node, uint32_t cell_num)
{
  return node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

// Return a pointer to the key of cell `cell_num`; the key sits at the start of the cell.
uint32_t *leaf_node_key(void *node, uint32_t cell_num)
{
  return leaf_node_cell(node, cell_num);
}

// Return a pointer to the value (the serialized row) of cell `cell_num`, just past its key.
void *leaf_node_value(void *node, uint32_t cell_num)
{
  return leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

// Turn a fresh page into an empty leaf node by zeroing its cell count.
void initialize_leaf_node(void *node) { *leaf_node_num_cells(node) = 0; }

// Print the row and leaf-node layout sizes, for the ".constants" meta command.
void print_constants()
{
  printf("ROW_SIZE: %d\n", ROW_SIZE);
  printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE);
  printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE);
  printf("LEAF_NODE_CELL_SIZE: %d\n", LEAF_NODE_CELL_SIZE);
  printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS);
  printf("LEAF_NODE_MAX_CELLS: %d\n", LEAF_NODE_MAX_CELLS);
}

// Print a leaf node's cell count and each cell's index and key, for the ".btree"
// meta command.
void print_leaf_node(void *node)
{
  uint32_t num_cells = *leaf_node_num_cells(node);
  printf("leaf (size %d)\n", num_cells);
  for (uint32_t i = 0; i < num_cells; i++)
  {
    uint32_t key = *leaf_node_key(node, i);
    printf("  - %d : %d\n", i, key);
  }
}

// Print one row to stdout in the tutorial's (id, username, email) format.
void print_row(Row *row)
{
  printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

// Copy a Row struct into the compact on-page (on-disk) layout at `destination`,
// field by field using the *_OFFSET constants.
void serialize_row(Row *source, void *destination)
{
  memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE);
  // strncpy (not memcpy) so the bytes after each string's terminator are zero-filled
  // rather than leftover stack garbage: keeps the on-disk pages deterministic.
  strncpy(destination + USERNAME_OFFSET, source->username, USERNAME_SIZE);
  strncpy(destination + EMAIL_OFFSET, source->email, EMAIL_SIZE);
}

// Inverse of serialize_row: read the compact on-page layout back into a Row struct.
void deserialize_row(void *source, Row *destination)
{
  memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE);
  memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE);
  memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

// Return a pointer to page `page_num`'s 4K cache, allocating it on a cache miss and
// loading its bytes from the file if that page already exists on disk. Touching a page
// past the current end grows pager->num_pages to include it.
// Exits the process if the page number is out of range or the read fails.
void *get_page(Pager *pager, uint32_t page_num)
{
  if (page_num > TABLE_MAX_PAGES)
  {
    printf("Tried to fetch page number out of bounds. %d > %d\n", page_num,
           TABLE_MAX_PAGES);
    exit(EXIT_FAILURE);
  }

  if (pager->pages[page_num] == NULL)
  {
    // Cache miss. Allocate memory and load from file.
    void *page = malloc(PAGE_SIZE);
    uint32_t num_pages = pager->file_length / PAGE_SIZE;

    // We might save a partial page at the end of the file
    if (pager->file_length % PAGE_SIZE)
    {
      num_pages += 1;
    }

    if (page_num <= num_pages)
    {
      lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
      ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
      if (bytes_read == -1)
      {
        printf("Error reading file: %d\n", errno);
        exit(EXIT_FAILURE);
      }
    }

    pager->pages[page_num] = page;

    if (page_num >= pager->num_pages)
    {
      pager->num_pages = page_num + 1;
    }
  }

  return pager->pages[page_num];
}

// Make a cursor pointing at cell 0 of the root node, for a scan from the beginning.
// end_of_table is set when the root has no cells, so a caller can loop on it straight
// away. The cursor is malloc'd; the caller frees it.
Cursor *table_start(Table *table)
{
  Cursor *cursor = malloc(sizeof(Cursor));
  cursor->table = table;
  cursor->page_num = table->root_page_num;
  cursor->cell_num = 0;

  void *root_node = get_page(table->pager, table->root_page_num);
  uint32_t num_cells = *leaf_node_num_cells(root_node);
  cursor->end_of_table = (num_cells == 0);

  return cursor;
}

// Make a cursor pointing one past the root node's last cell, which is where a new row
// gets written. Always end_of_table: there is nothing to read at this position.
// The cursor is malloc'd; the caller frees it.
Cursor *table_end(Table *table)
{
  Cursor *cursor = malloc(sizeof(Cursor));
  cursor->table = table;
  cursor->page_num = table->root_page_num;

  void *root_node = get_page(table->pager, table->root_page_num);
  uint32_t num_cells = *leaf_node_num_cells(root_node);
  cursor->cell_num = num_cells;
  cursor->end_of_table = true;

  return cursor;
}

// Return the byte address of the row the cursor points at: the value of cell
// `cell_num` in node `page_num`. This is the one place a cursor position becomes a
// pointer, so the VM never needs to know about the node layout.
void *cursor_value(Cursor *cursor)
{
  uint32_t page_num = cursor->page_num;
  void *page = get_page(cursor->table->pager, page_num);
  return leaf_node_value(page, cursor->cell_num);
}

// Step the cursor to the next cell in its node, setting end_of_table once it has moved
// past the node's last cell.
void cursor_advance(Cursor *cursor)
{
  uint32_t page_num = cursor->page_num;
  void *node = get_page(cursor->table->pager, page_num);

  cursor->cell_num += 1;
  if (cursor->cell_num >= (*leaf_node_num_cells(node)))
  {
    cursor->end_of_table = true;
  }
}

// Open (or create) the database file and build a Pager around it: records the file
// descriptor, length and page count, and starts with every page cache slot empty.
// Exits if the file cannot be opened or is not a whole number of pages.
Pager *pager_open(const char *filename)
{
  int fd = open(filename,
                O_RDWR |     // Read/Write mode
                    O_CREAT, // Create file if it does not exist
                S_IWUSR |    // User write permission
                    S_IRUSR  // User read permission
  );

  if (fd == -1)
  {
    printf("Unable to open file\n");
    exit(EXIT_FAILURE);
  }

  off_t file_length = lseek(fd, 0, SEEK_END);

  Pager *pager = malloc(sizeof(Pager));
  pager->file_descriptor = fd;
  pager->file_length = file_length;
  pager->num_pages = (file_length / PAGE_SIZE);

  if (file_length % PAGE_SIZE != 0)
  {
    printf("Db file is not a whole number of pages. Corrupt file.\n");
    exit(EXIT_FAILURE);
  }

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++)
  {
    pager->pages[i] = NULL;
  }

  return pager;
}

// Open the database: create the Pager for `filename` and a Table rooted at page 0.
// A brand-new (empty) file gets page 0 initialized as an empty leaf node.
Table *db_open(const char *filename)
{
  Pager *pager = pager_open(filename);
  uint32_t num_rows = pager->file_length / ROW_SIZE;

  Table *table = (Table *)malloc(sizeof(Table));
  table->pager = pager;
  table->root_page_num = 0;

  if (pager->num_pages == 0)
  {
    // New database file. Initialize page 0 as leaf node.
    void *root_node = get_page(pager, 0);
    initialize_leaf_node(root_node);
  }

  return table;
}

// Allocate an empty InputBuffer for getline() to fill in read_input().
InputBuffer *new_input_buffer()
{
  InputBuffer *input_buffer = (InputBuffer *)malloc(sizeof(InputBuffer));
  input_buffer->buffer = NULL;
  input_buffer->buffer_length = 0;
  input_buffer->input_length = 0;

  return input_buffer;
}

// Write the REPL prompt (no newline, so input appears on the same line).
void print_prompt() { printf("db > "); }

// Read one line from stdin into the buffer and strip the trailing newline.
// Exits on read error/EOF.
void read_input(InputBuffer *input_buffer)
{
  ssize_t bytes_read =
      getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);

  if (bytes_read <= 0)
  {
    printf("Error reading input\n");
    exit(EXIT_FAILURE);
  }

  // Ignore trailing newline
  input_buffer->input_length = bytes_read - 1;
  input_buffer->buffer[bytes_read - 1] = 0;
}

// Free an InputBuffer and the getline()-allocated string it owns.
void close_input_buffer(InputBuffer *input_buffer)
{
  free(input_buffer->buffer);
  free(input_buffer);
}

// Write the whole cached page `page_num` out to its offset in the database file.
// Pages are always flushed in full now that each one is a B-tree node.
// Exits on a null page, seek error or write error.
void pager_flush(Pager *pager, uint32_t page_num)
{
  if (pager->pages[page_num] == NULL)
  {
    printf("Tried to flush null page\n");
    exit(EXIT_FAILURE);
  }

  off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);

  if (offset == -1)
  {
    printf("Error seeking: %d\n", errno);
    exit(EXIT_FAILURE);
  }

  ssize_t bytes_written =
      write(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE);

  if (bytes_written == -1)
  {
    printf("Error writing: %d\n", errno);
    exit(EXIT_FAILURE);
  }
}

// Close the database: flush each of the pager's num_pages cached pages back to the file
// and free it, close the file descriptor, then free any remaining page caches, the Pager
// and the Table. Exits if the file cannot be closed.
void db_close(Table *table)
{
  Pager *pager = table->pager;

  for (uint32_t i = 0; i < pager->num_pages; i++)
  {
    if (pager->pages[i] == NULL)
    {
      continue;
    }
    pager_flush(pager, i);
    free(pager->pages[i]);
    pager->pages[i] = NULL;
  }

  int result = close(pager->file_descriptor);
  if (result == -1)
  {
    printf("Error closing db file.\n");
    exit(EXIT_FAILURE);
  }
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++)
  {
    void *page = pager->pages[i];
    if (page)
    {
      free(page);
      pager->pages[i] = NULL;
    }
  }
  free(pager);
  free(table);
}

// Handle a '.'-prefixed meta command: ".exit" cleans up and terminates, ".btree" prints
// the root leaf node, ".constants" prints the layout sizes; anything else reports back
// as unrecognized.
MetaCommandResult do_meta_command(InputBuffer *input_buffer, Table *table)
{
  if (strcmp(input_buffer->buffer, ".exit") == 0)
  {
    close_input_buffer(input_buffer);
    db_close(table);
    exit(EXIT_SUCCESS);
  }
  else if (strcmp(input_buffer->buffer, ".btree") == 0)
  {
    printf("Tree:\n");
    print_leaf_node(get_page(table->pager, 0));
    return META_COMMAND_SUCCESS;
  }
  else if (strcmp(input_buffer->buffer, ".constants") == 0)
  {
    printf("Constants:\n");
    print_constants();
    return META_COMMAND_SUCCESS;
  }
  else
  {
    return META_COMMAND_UNRECOGNIZED_COMMAND;
  }
}

// Parse "insert <id> <username> <email>" into `statement`, validating that all three
// arguments are present, the id is non-negative, and the strings fit their columns.
PrepareResult prepare_insert(InputBuffer *input_buffer, Statement *statement)
{
  statement->type = STATEMENT_INSERT;

  strtok(input_buffer->buffer, " "); // consume the "insert" keyword
  char *id_string = strtok(NULL, " ");
  char *username = strtok(NULL, " ");
  char *email = strtok(NULL, " ");

  if (id_string == NULL || username == NULL || email == NULL)
  {
    return PREPARE_SYNTAX_ERROR;
  }

  int id = atoi(id_string);
  if (id < 0)
  {
    return PREPARE_NEGATIVE_ID;
  }
  if (strlen(username) > COLUMN_USERNAME_SIZE)
  {
    return PREPARE_STRING_TOO_LONG;
  }
  if (strlen(email) > COLUMN_EMAIL_SIZE)
  {
    return PREPARE_STRING_TOO_LONG;
  }

  statement->row_to_insert.id = id;
  strcpy(statement->row_to_insert.username, username);
  strcpy(statement->row_to_insert.email, email);

  return PREPARE_SUCCESS;
}

// Front end: turn an input line into a Statement by dispatching on its keyword
// ("insert" -> prepare_insert, "select"), or report an unrecognized statement.
PrepareResult prepare_statement(InputBuffer *input_buffer, Statement *statement)
{
  if (strncmp(input_buffer->buffer, "insert", 6) == 0)
  {
    return prepare_insert(input_buffer, statement);
  }
  if (strcmp(input_buffer->buffer, "select") == 0)
  {
    statement->type = STATEMENT_SELECT;
    return PREPARE_SUCCESS;
  }

  return PREPARE_UNRECOGNIZED_STATEMENT;
}

// Insert a (key, row) cell into the cursor's leaf node at the cursor's cell position,
// shifting any later cells one slot right to make room. Exits if the node is already
// full, since leaf splitting is not implemented yet.
void leaf_node_insert(Cursor *cursor, uint32_t key, Row *value)
{
  void *node = get_page(cursor->table->pager, cursor->page_num);

  uint32_t num_cells = *leaf_node_num_cells(node);
  if (num_cells >= LEAF_NODE_MAX_CELLS)
  {
    // Node full
    printf("Need to implement splitting a leaf node.\n");
    exit(EXIT_FAILURE);
  }

  if (cursor->cell_num < num_cells)
  {
    // Make room for new cell
    for (uint32_t i = num_cells; i > cursor->cell_num; i--)
    {
      memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1), LEAF_NODE_CELL_SIZE);
    }
  }

  *(leaf_node_num_cells(node)) += 1;
  *(leaf_node_key(node, cursor->cell_num)) = key;
  serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

// Append the statement's row to the end of the root leaf node, keyed by its id, unless
// that node is already full.
ExecuteResult execute_insert(Statement *statement, Table *table)
{
  void *node = get_page(table->pager, table->root_page_num);
  if ((*leaf_node_num_cells(node) >= LEAF_NODE_MAX_CELLS))
  {
    return EXECUTE_TABLE_FULL;
  }

  Row *row_to_insert = &(statement->row_to_insert);
  Cursor *cursor = table_end(table);

  leaf_node_insert(cursor, row_to_insert->id, row_to_insert);

  free(cursor);

  return EXECUTE_SUCCESS;
}

// Scan every row in the table and print it.
ExecuteResult execute_select(Table *table)
{
  Cursor *cursor = table_start(table);

  Row row;
  while (!(cursor->end_of_table))
  {
    deserialize_row(cursor_value(cursor), &row);
    print_row(&row);
    cursor_advance(cursor);
  }

  free(cursor);

  return EXECUTE_SUCCESS;
}

// Virtual machine: dispatch a prepared statement to the matching execute_* routine.
ExecuteResult execute_statement(Statement *statement, Table *table)
{
  switch (statement->type)
  {
  case (STATEMENT_INSERT):
    return execute_insert(statement, table);
  case (STATEMENT_SELECT):
    return execute_select(table);
  }
  return EXECUTE_SUCCESS;
}

// REPL entry point: open the database named by argv[1], then loop reading a line,
// running it as a meta command or a prepared statement, and printing the result.
// This switch is the only place user-facing error strings live.
int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    printf("Must supply a database filename.\n");
    exit(EXIT_FAILURE);
  }

  char *filename = argv[1];
  Table *table = db_open(filename);

  InputBuffer *input_buffer = new_input_buffer();
  while (true)
  {
    print_prompt();
    read_input(input_buffer);

    if (input_buffer->buffer[0] == '.')
    {
      switch (do_meta_command(input_buffer, table))
      {
      case (META_COMMAND_SUCCESS):
        continue;
      case (META_COMMAND_UNRECOGNIZED_COMMAND):
        printf("Unrecognized command '%s'\n", input_buffer->buffer);
        continue;
      }
    }

    Statement statement;
    switch (prepare_statement(input_buffer, &statement))
    {
    case (PREPARE_SUCCESS):
      break;
    case (PREPARE_NEGATIVE_ID):
      printf("ID must be positive.\n");
      continue;
    case (PREPARE_STRING_TOO_LONG):
      printf("String is too long.\n");
      continue;
    case (PREPARE_SYNTAX_ERROR):
      printf("Syntax error. Could not parse statement.\n");
      continue;
    case (PREPARE_UNRECOGNIZED_STATEMENT):
      printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
      continue;
    }

    switch (execute_statement(&statement, table))
    {
    case (EXECUTE_SUCCESS):
      printf("Executed.\n");
      break;
    case (EXECUTE_TABLE_FULL):
      printf("Error: Table full.\n");
      break;
    }
  }
}