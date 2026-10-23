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
  EXECUTE_DUPLICATE_KEY,
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

const uint32_t ID_SIZE = size_of_attribute(Row, id);             // 4 bytes for id
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username); // 33 bytes for username (32 + 1 for null terminator)
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);       // 256 bytes for email (255 + 1 for null terminator)
const uint32_t ID_OFFSET = 0;                                    // id starts at offset 0
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;            // username starts after id (4 bytes for id)
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;   // email starts after username (4 bytes for id + 33 bytes for username)
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;  // total size of a row (4 + 33 + 256 = 293 bytes)

const uint32_t PAGE_SIZE = 4096; // 4KB page size
#define TABLE_MAX_PAGES 100      // Maximum number of pages in the table (100 * 4KB = 400KB)

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
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);                                             // 1 byte for node type
const uint32_t NODE_TYPE_OFFSET = 0;                                                         // node type is the first byte of the header
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);                                               // 1 byte for is root
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;                                              // is root is the second byte of the header
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);                                       // 4 bytes for parent pointer
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;                        // parent pointer is the third byte of the header
const uint8_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE; // total size of the common node header (1 + 1 + 4 = 6 bytes)

/*
 * Leaf Node Header Layout
 */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);                                // 4 bytes for number of cells
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;                       // number of cells is stored right after the common node header
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE; // total size of the leaf node header (6 + 4 = 10 bytes)

/*
 * Leaf Node Body Layout
 */
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);                                 // 4 bytes for key
const uint32_t LEAF_NODE_KEY_OFFSET = 0;                                              // key is the first part of the leaf node body
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;                                       // size of the value is the size of a row (293 bytes)
const uint32_t LEAF_NODE_VALUE_OFFSET = LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE;    // value is stored right after the key
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;       // 4 bytes for key + 293 bytes for value = 297 bytes per cell
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;         // Space available for cells in a leaf node (4096 - 10 = 4086 bytes)
const uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE; // Maximum number of cells in a leaf node (4086 / 297 = 13 cells)

const uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) / 2;                          // When splitting a leaf node, the right node gets half of the cells (7 cells)
const uint32_t LEAF_NODE_LEFT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) - LEAF_NODE_RIGHT_SPLIT_COUNT; // When splitting a leaf node, the left node gets the other half of the cells (7 cells)

/*
 * Internal Node Header Layout
 */
const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);                                                                     // 4 bytes for number of keys
const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;                                                            // number of keys is stored right after the common node header
const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);                                                                  // 4 bytes for right child pointer
const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET = INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;                     // right child pointer is stored right after the number of keys
const uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE; // total size of the internal node header (6 + 4 + 4 = 14 bytes)

/*
 * Internal Node Body Layout
 */
const uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);                                   // 4 bytes for key
const uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);                                 // 4 bytes for child pointer
const uint32_t INTERNAL_NODE_CELL_SIZE = INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE; // 8 bytes per cell

// Read a node's is-root flag from the second byte of its common header.
bool is_node_root(void *node)
{
  uint8_t value = *((uint8_t *)(node + IS_ROOT_OFFSET));
  return (bool)value;
}

// Write a node's is-root flag into the second byte of its common header.
void set_node_root(void *node, bool is_root)
{
  uint8_t value = is_root;
  *((uint8_t *)(node + IS_ROOT_OFFSET)) = value;
}

// Read a node's type (internal or leaf) from the first byte of its common header.
NodeType get_node_type(void *node)
{
  uint8_t value = *((uint8_t *)(node + NODE_TYPE_OFFSET));
  return (NodeType)value;
}

// Write a node's type (internal or leaf) into the first byte of its common header.
void set_node_type(void *node, NodeType type)
{
  uint8_t value = type;
  *((uint8_t *)(node + NODE_TYPE_OFFSET)) = value;
}

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

// Turn a fresh page into an empty leaf node: mark its type as NODE_LEAF, clear the
// is-root flag and zero its cell count.
void initialize_leaf_node(void *node)
{
  set_node_type(node, NODE_LEAF);
  set_node_root(node, false);
  *leaf_node_num_cells(node) = 0;
}

// Return a pointer to the internal node's key count, stored in its header right after
// the common node header.
uint32_t *internal_node_num_keys(void *node)
{
  return node + INTERNAL_NODE_NUM_KEYS_OFFSET;
}

// Return a pointer to the internal node's rightmost child page number, kept in the
// header rather than in a cell since an internal node has one more child than keys.
uint32_t *internal_node_right_child(void *node)
{
  return node + INTERNAL_NODE_RIGHT_CHILD_OFFSET;
}

// Return a pointer to the start of internal cell `cell_num` (a child page number
// followed by a key), counting from just after the internal node header.
uint32_t *internal_node_cell(void *node, uint32_t cell_num)
{
  return node + INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE;
}

// Return a pointer to child `child_num`'s page number: the child half of cell
// `child_num`, or the header's right child when child_num == num_keys. Exits if
// child_num is past the right child.
uint32_t *internal_node_child(void *node, uint32_t child_num)
{
  uint32_t num_keys = *internal_node_num_keys(node);
  if (child_num > num_keys)
  {
    printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys);
    exit(EXIT_FAILURE);
  }
  else if (child_num == num_keys)
  {
    return internal_node_right_child(node);
  }
  else
  {
    return internal_node_cell(node, child_num);
  }
}

// Return a pointer to key `key_num`, stored in its cell right after the child page
// number.
uint32_t *internal_node_key(void *node, uint32_t key_num)
{
  return (void *)internal_node_cell(node, key_num) + INTERNAL_NODE_CHILD_SIZE;
}

// Turn a page into an empty internal node: mark its type as NODE_INTERNAL, clear the
// is-root flag and zero its key count.
void initialize_internal_node(void *node)
{
  set_node_type(node, NODE_INTERNAL);
  set_node_root(node, false);
  *internal_node_num_keys(node) = 0;
}

// Return the largest key in a node: its last key, whether it is an internal or a leaf
// node.
uint32_t get_node_max_key(void *node)
{
  switch (get_node_type(node))
  {
  case NODE_INTERNAL:
    return *internal_node_key(node, *internal_node_num_keys(node) - 1);
  case NODE_LEAF:
    return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
  }
}

// Return the page number to use for a new page. Until free pages are recycled, new
// pages always go onto the end of the database file.
uint32_t get_unused_page_num(Pager *pager) { return pager->num_pages; }

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

// Print two spaces per indentation level, for nesting the ".btree" output.
void indent(uint32_t level)
{
  for (uint32_t i = 0; i < level; i++)
  {
    printf("  ");
  }
}

// Recursively print the tree rooted at `page_num` for the ".btree" meta command: a
// leaf lists its keys, an internal node prints each child subtree followed by the key
// that separates it from the next.
void print_tree(Pager *pager, uint32_t page_num, uint32_t indentation_level)
{
  void *node = get_page(pager, page_num);
  uint32_t num_keys, child;

  switch (get_node_type(node))
  {
  case (NODE_LEAF):
    num_keys = *leaf_node_num_cells(node);
    indent(indentation_level);
    printf("- leaf (size %d)\n", num_keys);
    for (uint32_t i = 0; i < num_keys; i++)
    {
      indent(indentation_level + 1);
      printf("- %d\n", *leaf_node_key(node, i));
    }
    break;
  case (NODE_INTERNAL):
    num_keys = *internal_node_num_keys(node);
    indent(indentation_level);
    printf("- internal (size %d)\n", num_keys);
    for (uint32_t i = 0; i < num_keys; i++)
    {
      child = *internal_node_child(node, i);
      print_tree(pager, child, indentation_level + 1);

      indent(indentation_level + 1);
      printf("- key %d\n", *internal_node_key(node, i));
    }
    child = *internal_node_right_child(node);
    print_tree(pager, child, indentation_level + 1);
    break;
  }
}

// Print one row to stdout in the tutorial's (id, username, email) format.
void print_row(Row *row)
{
  printf("(%d, %s, %s)\n", row->id, row->username, row->email);
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

// Binary-search leaf node `page_num` for `key` and return a cursor at its cell, or, if
// the key is absent, at the cell where it would be inserted to keep keys sorted.
// The cursor is malloc'd; the caller frees it.
Cursor *leaf_node_find(Table *table, uint32_t page_num, uint32_t key)
{
  void *node = get_page(table->pager, page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  Cursor *cursor = malloc(sizeof(Cursor));
  cursor->table = table;
  cursor->page_num = page_num;

  // Binary search
  uint32_t min_index = 0;
  uint32_t one_past_max_index = num_cells;
  while (one_past_max_index != min_index)
  {
    uint32_t index = (min_index + one_past_max_index) / 2;
    uint32_t key_at_index = *leaf_node_key(node, index);
    if (key == key_at_index)
    {
      cursor->cell_num = index;
      return cursor;
    }
    if (key < key_at_index)
    {
      one_past_max_index = index;
    }
    else
    {
      min_index = index + 1;
    }
  }

  cursor->cell_num = min_index;
  return cursor;
}

// Binary-search internal node `page_num` for the child whose range covers `key` (the
// first child whose max key is >= key, else the right child), then recurse into it
// until a leaf, returning leaf_node_find's cursor there. The cursor is malloc'd; the
// caller frees it.
Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
  void* node = get_page(table->pager, page_num);
  uint32_t num_keys = *internal_node_num_keys(node);

  /* Binary search to find index of child to search */
  uint32_t min_index = 0;
  uint32_t max_index = num_keys; /* there is one more child than key */

  while (min_index != max_index) {
    uint32_t index = (min_index + max_index) / 2;
    uint32_t key_to_right = *internal_node_key(node, index);
    if (key_to_right >= key) {
      max_index = index;
    } else {
      min_index = index + 1;
    }
  }
  uint32_t child_num = *internal_node_child(node, min_index);
  void* child = get_page(table->pager, child_num);
  switch (get_node_type(child)) {
    case NODE_LEAF:
      return leaf_node_find(table, child_num, key);
    case NODE_INTERNAL:
      return internal_node_find(table, child_num, key);
  }
}

// Return a cursor at the position of `key` in the table, or where it should be inserted
// if it is not present: search the root directly if it is a leaf, otherwise descend
// from it through internal_node_find to the leaf that should hold the key.
Cursor *table_find(Table *table, uint32_t key)
{
  uint32_t root_page_num = table->root_page_num;
  void *root_node = get_page(table->pager, root_page_num);

  if (get_node_type(root_node) == NODE_LEAF)
  {
    return leaf_node_find(table, root_page_num, key);
  }
  else
  {
    return internal_node_find(table, root_page_num, key);
  }
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
    set_node_root(root_node, true);
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
// the whole tree from the root, ".constants" prints the layout sizes; anything else
// reports back as unrecognized.
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
    print_tree(table->pager, 0, 0);
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

// Split the root: copy the old root to a new page as the left child, then rewrite the
// root page in place as an internal node with one key pointing at the left child and
// `right_child_page_num`. The root stays on page root_page_num.
void create_new_root(Table *table, uint32_t right_child_page_num)
{
  /*
  Handle splitting the root.
  Old root copied to new page, becomes left child.
  Address of right child passed in.
  Re-initialize root page to contain the new root node.
  New root node points to two children.
  */

  void *root = get_page(table->pager, table->root_page_num);
  void *right_child = get_page(table->pager, right_child_page_num);
  uint32_t left_child_page_num = get_unused_page_num(table->pager);
  void *left_child = get_page(table->pager, left_child_page_num);

  /* Left child has data copied from old root */
  memcpy(left_child, root, PAGE_SIZE);
  set_node_root(left_child, false);

  /* Root node is a new internal node with one key and two children */
  initialize_internal_node(root);
  set_node_root(root, true);
  *internal_node_num_keys(root) = 1;
  *internal_node_child(root, 0) = left_child_page_num;
  uint32_t left_child_max_key = get_node_max_key(left_child);
  *internal_node_key(root, 0) = left_child_max_key;
  *internal_node_right_child(root) = right_child_page_num;
}

// Split a full leaf: move the upper half of its cells (plus the new one) into a fresh
// leaf on a new page, insert the new cell at the cursor's position on whichever side
// it falls, then create a new root if the old leaf was the root. Exits if it was not,
// since updating a parent is not implemented yet.
void leaf_node_split_and_insert(Cursor *cursor, uint32_t key, Row *value)
{
  /*
  Create a new node and move half the cells over.
  Insert the new value in one of the two nodes.
  Update parent or create a new parent.
  */
  void *old_node = get_page(cursor->table->pager, cursor->page_num);
  uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
  void *new_node = get_page(cursor->table->pager, new_page_num);
  initialize_leaf_node(new_node);

  /*
  All existing keys plus new key should be divided
  evenly between old (left) and new (right) nodes.
  Starting from the right, move each key to correct position.
  */
  for (int32_t i = (int32_t)LEAF_NODE_MAX_CELLS; i >= 0; i--)
  {
    void *destination_node;
    if (i >= LEAF_NODE_LEFT_SPLIT_COUNT)
    {
      destination_node = new_node;
    }
    else
    {
      destination_node = old_node;
    }
    uint32_t index_within_node = i % LEAF_NODE_LEFT_SPLIT_COUNT;
    void *destination = leaf_node_cell(destination_node, index_within_node);

    if (i == cursor->cell_num)
    {
      serialize_row(value, destination);
    }
    else if (i > cursor->cell_num)
    {
      memcpy(destination, leaf_node_cell(old_node, i - 1), LEAF_NODE_CELL_SIZE);
    }
    else
    {
      memcpy(destination, leaf_node_cell(old_node, i), LEAF_NODE_CELL_SIZE);
    }
  }

  /* Update cell count on both leaf nodes */
  *(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
  *(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;

  if (is_node_root(old_node))
  {
    return create_new_root(cursor->table, new_page_num);
  }
  else
  {
    printf("Need to implement updating parent after split\n");
    exit(EXIT_FAILURE);
  }
}

// Insert a (key, row) cell into the cursor's leaf node at the cursor's cell position,
// shifting any later cells one slot right to make room. If the node is already full it
// hands off to leaf_node_split_and_insert instead.
void leaf_node_insert(Cursor *cursor, uint32_t key, Row *value)
{
  void *node = get_page(cursor->table->pager, cursor->page_num);

  uint32_t num_cells = *leaf_node_num_cells(node);
  if (num_cells >= LEAF_NODE_MAX_CELLS)
  {
    // Node full
    leaf_node_split_and_insert(cursor, key, value);
    return;
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

// Insert the statement's row keyed by its id, at the sorted position table_find returns;
// leaf_node_insert splits the leaf if it is full. Fails with EXECUTE_DUPLICATE_KEY if
// that id is already present. The duplicate check still reads the root page as a leaf,
// so it only works while the root is a leaf.
ExecuteResult execute_insert(Statement *statement, Table *table)
{
  void *node = get_page(table->pager, table->root_page_num);
  uint32_t num_cells = (*leaf_node_num_cells(node));

  Row *row_to_insert = &(statement->row_to_insert);
  uint32_t key_to_insert = row_to_insert->id;
  Cursor *cursor = table_find(table, key_to_insert);

  if (cursor->cell_num < num_cells)
  {
    uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
    if (key_at_index == key_to_insert)
    {
      return EXECUTE_DUPLICATE_KEY;
    }
  }

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
    case (EXECUTE_DUPLICATE_KEY):
      printf("Error: Duplicate key.\n");
      break;
    case (EXECUTE_TABLE_FULL):
      printf("Error: Table full.\n");
      break;
    }
  }
}