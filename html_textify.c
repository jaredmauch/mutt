/*
 * Copyright (C) 2024 Jared <jared@example.com>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "config.h"
#include "mutt.h"
#include "html_textify.h"
#include "lib.h"
#include "globals.h"
#include "buffer.h"
#include <ctype.h>
#include "mutt_curses.h"

#ifdef HAVE_LIBXML2
#include <libxml2/libxml/parser.h>
#include <libxml2/libxml/tree.h>
#include <libxml2/libxml/HTMLparser.h>
#include <libxml2/libxml/HTMLtree.h>

/* Forward declarations */
static char *collapse_blank_lines(const char *input);

/* Table structure for preserving table formatting */
typedef struct table_cell {
  char *content;
  int colspan;
  int rowspan;
  struct table_cell *next;
} table_cell_t;

typedef struct table_row {
  table_cell_t *cells;
  int cell_count;
  struct table_row *next;
} table_row_t;

typedef struct table_data {
  table_row_t *rows;
  int row_count;
  int max_cols;
  int *col_widths;
} table_data_t;

/* Extract text content from a cell node */
static char *mutt_extract_cell_text(xmlNodePtr node)
{
  if (!node)
    return NULL;

  BUFFER *cell_buffer = mutt_buffer_new();
  
  for (xmlNodePtr child = node->children; child; child = child->next)
  {
    if (child->type == XML_TEXT_NODE && child->content)
    {
      char *text = (char *)child->content;
      /* Skip leading/trailing whitespace */
      while (*text && isspace((unsigned char)*text))
        text++;
      
      if (*text)
      {
        mutt_buffer_addstr(cell_buffer, text);
        mutt_buffer_addch(cell_buffer, ' ');
      }
    }
    else if (child->type == XML_ELEMENT_NODE)
    {
      /* Recursively extract text from child elements */
      char *child_text = mutt_extract_cell_text(child);
      if (child_text)
      {
        mutt_buffer_addstr(cell_buffer, child_text);
        FREE(&child_text);
      }
    }
  }
  
  char *result = safe_strdup(mutt_b2s(cell_buffer));
  mutt_buffer_free(&cell_buffer);
  return result;
}

/* Extract table structure from HTML */
static void mutt_html_extract_table(xmlNodePtr node, table_data_t *table)
{
  if (!node || !table)
    return;

  if (node->type == XML_ELEMENT_NODE && node->name)
  {
    if (strcmp((char *)node->name, "table") == 0)
    {
      /* Initialize table */
      table->rows = NULL;
      table->row_count = 0;
      table->max_cols = 0;
      table->col_widths = NULL;
    }
    else if (strcmp((char *)node->name, "tr") == 0)
    {
      /* Create new row */
      table_row_t *row = safe_calloc(1, sizeof(table_row_t));
      row->cells = NULL;
      row->cell_count = 0;
      
      /* Add to table */
      if (!table->rows)
        table->rows = row;
      else
      {
        table_row_t *last = table->rows;
        while (last->next)
          last = last->next;
        last->next = row;
      }
      table->row_count++;
    }
    else if (strcmp((char *)node->name, "td") == 0 || 
             strcmp((char *)node->name, "th") == 0)
    {
      /* Create new cell */
      table_cell_t *cell = safe_calloc(1, sizeof(table_cell_t));
      cell->content = mutt_extract_cell_text(node);
      cell->colspan = 1;
      cell->rowspan = 1;
      
      /* Get colspan and rowspan attributes */
      for (xmlAttrPtr attr = node->properties; attr; attr = attr->next)
      {
        if (strcmp((char *)attr->name, "colspan") == 0 && attr->children)
          cell->colspan = atoi((char *)attr->children->content);
        else if (strcmp((char *)attr->name, "rowspan") == 0 && attr->children)
          cell->rowspan = atoi((char *)attr->children->content);
      }
      
      /* Add to current row */
      if (table->rows)
      {
        table_row_t *current_row = table->rows;
        for (int i = 0; i < table->row_count - 1; i++)
          current_row = current_row->next;
        
        if (!current_row->cells)
          current_row->cells = cell;
        else
        {
          table_cell_t *last = current_row->cells;
          while (last->next)
            last = last->next;
          last->next = cell;
        }
        current_row->cell_count++;
      }
    }
  }
  
  /* Process child nodes */
  for (xmlNodePtr child = node->children; child; child = child->next)
  {
    mutt_html_extract_table(child, table);
  }
}

/* Render table as ASCII text */
static void mutt_render_table(table_data_t *table, BUFFER *buffer)
{
  if (!table || !table->rows)
    return;

  /* Calculate column widths */
  table->max_cols = 0;
  table_row_t *row = table->rows;
  while (row)
  {
    if (row->cell_count > table->max_cols)
      table->max_cols = row->cell_count;
    row = row->next;
  }
  
  if (table->max_cols == 0)
    return;

  table->col_widths = safe_calloc(table->max_cols, sizeof(int));
  
  /* Calculate maximum width for each column */
  row = table->rows;
  while (row)
  {
    int col = 0;
    table_cell_t *cell = row->cells;
    while (cell && col < table->max_cols)
    {
      int cell_width = cell->content ? strlen(cell->content) : 0;
      if (cell_width > table->col_widths[col])
        table->col_widths[col] = cell_width;
      col += cell->colspan;
      cell = cell->next;
    }
    row = row->next;
  }
  
  /* Render table */
  row = table->rows;
  while (row)
  {
    /* Top border */
    mutt_buffer_addch(buffer, '+');
    for (int col = 0; col < table->max_cols; col++)
    {
      for (int i = 0; i < table->col_widths[col] + 2; i++)
        mutt_buffer_addch(buffer, '-');
      mutt_buffer_addch(buffer, '+');
    }
    mutt_buffer_addch(buffer, '\n');
    
    /* Cell content */
    mutt_buffer_addch(buffer, '|');
    int col = 0;
    table_cell_t *cell = row->cells;
    while (cell && col < table->max_cols)
    {
      mutt_buffer_addch(buffer, ' ');
      if (cell->content)
        mutt_buffer_addstr(buffer, cell->content);
      else
        mutt_buffer_addstr(buffer, "");
      
      /* Pad to column width */
      int content_len = cell->content ? strlen(cell->content) : 0;
      for (int i = content_len; i < table->col_widths[col]; i++)
        mutt_buffer_addch(buffer, ' ');
      mutt_buffer_addch(buffer, ' ');
      mutt_buffer_addch(buffer, '|');
      
      col += cell->colspan;
      cell = cell->next;
    }
    mutt_buffer_addch(buffer, '\n');
    
    row = row->next;
  }
  
  /* Bottom border */
  mutt_buffer_addch(buffer, '+');
  for (int col = 0; col < table->max_cols; col++)
  {
    for (int i = 0; i < table->col_widths[col] + 2; i++)
      mutt_buffer_addch(buffer, '-');
    mutt_buffer_addch(buffer, '+');
  }
  mutt_buffer_addch(buffer, '\n');
}

/* Free table data structure */
static void mutt_free_table(table_data_t *table)
{
  if (!table)
    return;

  table_row_t *row = table->rows;
  while (row)
  {
    table_cell_t *cell = row->cells;
    while (cell)
    {
      table_cell_t *next_cell = cell->next;
      FREE(&cell->content);
      FREE(&cell);
      cell = next_cell;
    }
    table_row_t *next_row = row->next;
    FREE(&row);
    row = next_row;
  }
  
  FREE(&table->col_widths);
}

/* Detect if a table is used for layout rather than data */
static int is_layout_table(xmlNodePtr node) {
  /* Detects if a table is likely used for layout */
  xmlChar *role = xmlGetProp(node, (const xmlChar *)"role");
  xmlChar *border = xmlGetProp(node, (const xmlChar *)"border");
  int is_layout = 0;
  if (role && strcasecmp((char *)role, "presentation") == 0)
    is_layout = 1;
  if (border && strcmp((char *)border, "0") == 0)
    is_layout = 1;
  /* Count rows and columns */
  int row_count = 0, col_count = 0;
  for (xmlNodePtr tr = node->children; tr; tr = tr->next) {
    if (tr->type == XML_ELEMENT_NODE && strcmp((char *)tr->name, "tr") == 0) {
      row_count++;
      int this_cols = 0;
      for (xmlNodePtr td = tr->children; td; td = td->next) {
        if (td->type == XML_ELEMENT_NODE &&
            (strcmp((char *)td->name, "td") == 0 || strcmp((char *)td->name, "th") == 0))
          this_cols++;
      }
      if (this_cols > col_count) col_count = this_cols;
    }
  }
  if (row_count <= 1 || col_count <= 1)
    is_layout = 1;
  if (role) xmlFree(role);
  if (border) xmlFree(border);
  return is_layout;
}

/* Wrap text to specified width and append to buffer */
static void wrap_and_append(BUFFER *buffer, const char *text, int width) {
  int col = 0;
  const char *p = text;
  while (*p) {
    if (*p == '\n') {
      mutt_buffer_addch(buffer, '\n');
      col = 0;
      p++;
      continue;
    }
    if (col >= width && *p != ' ') {
      mutt_buffer_addch(buffer, '\n');
      col = 0;
    }
    mutt_buffer_addch(buffer, *p);
    col++;
    p++;
  }
  if (col > 0) mutt_buffer_addch(buffer, '\n');
}

/* Recursively extract text from HTML nodes */
static void mutt_html_extract_text(xmlNodePtr node, BUFFER *buffer)
{
  if (!node || !buffer)
    return;

  /* Process different node types */
  switch (node->type)
  {
    case XML_TEXT_NODE:
      /* Add text content */
      if (node->content)
      {
        char *text = (char *)node->content;
        /* Skip leading/trailing whitespace */
        while (*text && isspace((unsigned char)*text))
          text++;
        
        if (*text)
        {
          mutt_buffer_addstr(buffer, text);
          /* Add space after text nodes */
          mutt_buffer_addch(buffer, ' ');
        }
      }
      break;
      
    case XML_ELEMENT_NODE:
      /* Handle specific HTML elements */
      if (node->name)
      {
        if (strcmp((char *)node->name, "table") == 0)
        {
          if (is_layout_table(node)) {
            /* Extract all text from the table and wrap it */
            BUFFER *tmp = mutt_buffer_new();
            mutt_html_extract_text(node->children, tmp);
            int width = (MuttIndexWindow && MuttIndexWindow->cols > 0) ? MuttIndexWindow->cols : 72;
            wrap_and_append(buffer, mutt_b2s(tmp), width);
            mutt_buffer_free(&tmp);
            /* Return early for layout tables since we've processed all children */
            return;
          } else {
            /* Handle table specially */
            table_data_t table = {0};
            mutt_html_extract_table(node, &table);
            mutt_render_table(&table, buffer);
            mutt_free_table(&table);
          }
          /* Don't return - continue processing siblings */
        }
        else if (strcmp((char *)node->name, "img") == 0)
        {
          /* Handle img tags - display alt text if available */
          xmlChar *alt = xmlGetProp(node, (const xmlChar *)"alt");
          if (alt && strlen((char *)alt) > 0)
          {
            mutt_buffer_addstr(buffer, "[Image: ");
            mutt_buffer_addstr(buffer, (char *)alt);
            mutt_buffer_addstr(buffer, "]");
          }
          else
          {
            mutt_buffer_addstr(buffer, "[Image]");
          }
          if (alt) xmlFree(alt);
        }
        else if (strcmp((char *)node->name, "br") == 0 ||
                 strcmp((char *)node->name, "p") == 0 ||
                 strcmp((char *)node->name, "div") == 0 ||
                 strcmp((char *)node->name, "h1") == 0 ||
                 strcmp((char *)node->name, "h2") == 0 ||
                 strcmp((char *)node->name, "h3") == 0 ||
                 strcmp((char *)node->name, "h4") == 0 ||
                 strcmp((char *)node->name, "h5") == 0 ||
                 strcmp((char *)node->name, "h6") == 0 ||
                 strcmp((char *)node->name, "li") == 0 ||
                 strcmp((char *)node->name, "tr") == 0)
        {
          /* Add newline for block elements */
          mutt_buffer_addch(buffer, '\n');
        }
        else if (strcmp((char *)node->name, "script") == 0 ||
                 strcmp((char *)node->name, "style") == 0 ||
                 strcmp((char *)node->name, "meta") == 0 ||
                 strcmp((char *)node->name, "link") == 0 ||
                 strcmp((char *)node->name, "title") == 0)
        {
          /* Skip these elements entirely */
          return;
        }
      }
      
      /* Process child nodes */
      for (xmlNodePtr child = node->children; child; child = child->next)
      {
        mutt_html_extract_text(child, buffer);
      }
      break;
      
    default:
      /* Process child nodes for other node types */
      for (xmlNodePtr child = node->children; child; child = child->next)
      {
        mutt_html_extract_text(child, buffer);
      }
      break;
  }
}

/* HTML to text conversion using libxml2 with relaxed parsing */
char *mutt_html_to_text(const char *html_content, size_t html_len)
{
  if (!html_content || html_len == 0)
    return NULL;

  dprint(1, (debugfile, "mutt_html_to_text: Starting HTML textification for %zu bytes\n", html_len));

  /* Simple check for basic HTML structure */
  if (html_len < 10) {
    dprint(1, (debugfile, "mutt_html_to_text: HTML content too short\n"));
    return NULL;
  }

  /* Create HTML parser context with very relaxed parsing options */
  htmlDocPtr doc = htmlReadMemory(html_content, html_len, 
                                 "input.html", NULL, 
                                 HTML_PARSE_RECOVER | 
                                 HTML_PARSE_NOERROR | 
                                 HTML_PARSE_NOWARNING | 
                                 HTML_PARSE_NONET |
                                 HTML_PARSE_NOBLANKS |
                                 XML_PARSE_NOCDATA);
  if (!doc)
  {
    dprint(1, (debugfile, "mutt_html_to_text: Failed to parse HTML document\n"));
    return NULL;
  }

  dprint(1, (debugfile, "mutt_html_to_text: HTML parsing successful\n"));

  /* Get the root element */
  xmlNodePtr root = xmlDocGetRootElement(doc);
  if (!root)
  {
    dprint(1, (debugfile, "mutt_html_to_text: No root element found\n"));
    xmlFreeDoc(doc);
    return NULL;
  }

  /* Extract text content */
  BUFFER *text_buffer = mutt_buffer_new();
  mutt_html_extract_text(root, text_buffer);
  
  /* Clean up */
  xmlFreeDoc(doc);
  
  /* Return the extracted text */
  char *result = safe_strdup(mutt_b2s(text_buffer));
  mutt_buffer_free(&text_buffer);
  
  if (!result || strlen(result) == 0)
  {
    dprint(1, (debugfile, "mutt_html_to_text: No text content extracted\n"));
    FREE(&result);
    return NULL;
  }
  
  // Collapse more than 2 blank lines in a row
  char *collapsed = collapse_blank_lines(result);
  FREE(&result);
  result = collapsed;

  dprint(1, (debugfile, "mutt_html_to_text: Successfully extracted %zu characters of text\n", strlen(result)));
  return result;
}

#else /* !HAVE_LIBXML2 */

/* Fallback implementation when libxml2 is not available */
char *mutt_html_to_text(const char *html_content, size_t html_len)
{
  if (!html_content || html_len == 0)
    return NULL;

  dprint(1, (debugfile, "mutt_html_to_text: libxml2 not available, returning NULL\n"));
  return NULL;
}

#endif /* HAVE_LIBXML2 */ 

// Collapse more than 2 consecutive blank lines in a string
static char *collapse_blank_lines(const char *input) {
  if (!input) return NULL;
  size_t len = strlen(input);
  char *out = safe_malloc(len + 1);
  size_t i = 0, j = 0;
  int newline_count = 0;
  while (input[i]) {
    if (input[i] == '\n') {
      newline_count++;
      if (newline_count <= 2) {
        out[j++] = input[i];
      }
    } else {
      newline_count = 0;
      out[j++] = input[i];
    }
    i++;
  }
  out[j] = '\0';
  return out;
} 
