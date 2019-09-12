/* Copyright (C) 2004 Justin Hibbits <jrh29@po.cwru.edu>
 * Copyright (C) 2004 Thomer M. Gil <mutt@thomer.com>
 * Copyright (C) 2015-2016 Richard Russon <rich@flatcap.org>
 * Copyright (C) 2016-2017 Kevin J. McCarthy <kevin@8t8.us>
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
 *     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "buffy.h"
#include "keymap.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "sort.h"

/* Previous values for some sidebar config */
static short PreviousSort = SORT_ORDER;  /* sidebar_sort_method */

/**
 * struct sidebar_entry - Info about folders in the sidebar
 */
typedef struct sidebar_entry
{
  char         box[STRING];     /* formatted mailbox name */
  BUFFY       *buffy;
  short        is_hidden;
} SBENTRY;

static int EntryCount = 0;
static int EntryLen   = 0;
static SBENTRY **Entries = NULL;

static int TopIndex = -1;    /* First mailbox visible in sidebar */
static int OpnIndex = -1;    /* Current (open) mailbox */
static int HilIndex = -1;    /* Highlighted mailbox */
static int BotIndex = -1;    /* Last mailbox visible in sidebar */

static int select_next (void);


/**
 * cb_format_str - Create the string to show in the sidebar
 * @dest:        Buffer in which to save string
 * @destlen:     Buffer length
 * @col:         Starting column, UNUSED
 * @op:          printf-like operator, e.g. 'B'
 * @src:         printf-like format string
 * @prefix:      Field formatting string, UNUSED
 * @ifstring:    If condition is met, display this string
 * @elsestring:  Otherwise, display this string
 * @data:        Pointer to our sidebar_entry
 * @flags:       Format flags, e.g. MUTT_FORMAT_OPTIONAL
 *
 * cb_format_str is a callback function for mutt_FormatString.  It understands
 * six operators. '%B' : Mailbox name, '%F' : Number of flagged messages,
 * '%N' : Number of new messages, '%S' : Size (total number of messages),
 * '%!' : Icon denoting number of flagged messages.
 * '%n' : N if folder has new mail, blank otherwise.
 *
 * Returns: src (unchanged)
 */
static const char *cb_format_str(char *dest, size_t destlen, size_t col, int cols, char op,
                                 const char *src, const char *prefix, const char *ifstring,
                                 const char *elsestring, unsigned long data, format_flag flags)
{
  SBENTRY *sbe = (SBENTRY *) data;
  unsigned int optional;
  char fmt[STRING];

  if (!sbe || !dest)
    return src;

  dest[0] = 0;	/* Just in case there's nothing to do */

  BUFFY *b = sbe->buffy;
  if (!b)
    return src;

  int c = Context && (mutt_strcmp (Context->realpath, b->realpath) == 0);

  optional = flags & MUTT_FORMAT_OPTIONAL;

  switch (op)
  {
    case 'B':
      mutt_format_s (dest, destlen, prefix, sbe->box);
      break;

    case 'd':
      if (!optional)
      {
        snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
        snprintf (dest, destlen, fmt, c ? Context->deleted : 0);
      }
      else if ((c && Context->deleted == 0) || !c)
        optional = 0;
      break;

    case 'F':
      if (!optional)
      {
        snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
        snprintf (dest, destlen, fmt, b->msg_flagged);
      }
      else if (b->msg_flagged == 0)
        optional = 0;
      break;

    case 'L':
      if (!optional)
      {
        snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
        snprintf (dest, destlen, fmt, c ? Context->vcount : b->msg_count);
      }
      else if ((c && Context->vcount == b->msg_count) || !c)
        optional = 0;
      break;

    case 'N':
      if (!optional)
      {
        snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
        snprintf (dest, destlen, fmt, b->msg_unread);
      }
      else if (b->msg_unread == 0)
        optional = 0;
      break;

    case 'n':
      if (!optional)
      {
        snprintf (fmt, sizeof (fmt), "%%%sc", prefix);
        snprintf (dest, destlen, fmt, b->new ? 'N' : ' ');
      }
      else if (b->new == 0)
        optional = 0;
      break;

    case 'S':
      if (!optional)
      {
        snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
        snprintf (dest, destlen, fmt, b->msg_count);
      }
      else if (b->msg_count == 0)
        optional = 0;
      break;

    case 't':
      if (!optional)
      {
        snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
        snprintf (dest, destlen, fmt, c ? Context->tagged : 0);
      }
      else if ((c && Context->tagged == 0) || !c)
        optional = 0;
      break;

    case '!':
      if (b->msg_flagged == 0)
        mutt_format_s (dest, destlen, prefix, "");
      else if (b->msg_flagged == 1)
        mutt_format_s (dest, destlen, prefix, "!");
      else if (b->msg_flagged == 2)
        mutt_format_s (dest, destlen, prefix, "!!");
      else
      {
        snprintf (fmt, sizeof (fmt), "%d!", b->msg_flagged);
        mutt_format_s (dest, destlen, prefix, fmt);
      }
      break;
  }

  if (optional)
    mutt_FormatString (dest, destlen, col, SidebarWidth, ifstring,   cb_format_str, (unsigned long) sbe, flags);
  else if (flags & MUTT_FORMAT_OPTIONAL)
    mutt_FormatString (dest, destlen, col, SidebarWidth, elsestring, cb_format_str, (unsigned long) sbe, flags);

  /* We return the format string, unchanged */
  return src;
}

/**
 * make_sidebar_entry - Turn mailbox data into a sidebar string
 * @buf:     Buffer in which to save string
 * @buflen:  Buffer length
 * @width:   Desired width in screen cells
 * @box:     Mailbox name
 * @b:       Mailbox object
 *
 * Take all the relevant mailbox data and the desired screen width and then get
 * mutt_FormatString to do the actual work. mutt_FormatString will callback to
 * us using cb_format_str() for the sidebar specific formatting characters.
 */
static void make_sidebar_entry (char *buf, unsigned int buflen, int width, const char *box,
                                SBENTRY *sbe)
{
  if (!buf || !box || !sbe)
    return;

  strfcpy (sbe->box, box, sizeof (sbe->box));

  mutt_FormatString (buf, buflen, 0, width, NONULL(SidebarFormat), cb_format_str, (unsigned long) sbe, 0);

  /* Force string to be exactly the right width */
  int w = mutt_strwidth (buf);
  int s = mutt_strlen (buf);
  width = MIN(buflen, width);
  if (w < width)
  {
    /* Pad with spaces */
    memset (buf + s, ' ', width - w);
    buf[s + width - w] = 0;
  }
  else if (w > width)
  {
    /* Truncate to fit */
    int len = mutt_wstr_trunc (buf, buflen, width, NULL);
    buf[len] = 0;
  }
}

/**
 * cb_qsort_sbe - qsort callback to sort SBENTRYs
 * @a: First  SBENTRY to compare
 * @b: Second SBENTRY to compare
 *
 * Returns:
 *	-1: a precedes b
 *	 0: a and b are identical
 *	 1: b precedes a
 */
static int cb_qsort_sbe (const void *a, const void *b)
{
  const SBENTRY *sbe1 = *(const SBENTRY **) a;
  const SBENTRY *sbe2 = *(const SBENTRY **) b;
  BUFFY *b1 = sbe1->buffy;
  BUFFY *b2 = sbe2->buffy;

  int result = 0;

  switch ((SidebarSortMethod & SORT_MASK))
  {
    case SORT_COUNT:
      result = (b2->msg_count - b1->msg_count);
      break;
    case SORT_UNREAD:
      result = (b2->msg_unread - b1->msg_unread);
      break;
    case SORT_FLAGGED:
      result = (b2->msg_flagged - b1->msg_flagged);
      break;
    case SORT_PATH:
      result = mutt_strcasecmp (mutt_b2s (b1->pathbuf), mutt_b2s (b2->pathbuf));
      break;
  }

  if (SidebarSortMethod & SORT_REVERSE)
    result = -result;

  return result;
}

/**
 * update_entries_visibility - Should a sidebar_entry be displayed in the sidebar
 *
 * For each SBENTRY in the Entries array, check whether we should display it.
 * This is determined by several criteria.  If the BUFFY:
 *	is the currently open mailbox
 *	is the currently highlighted mailbox
 *	has unread messages
 *	has flagged messages
 *	is whitelisted
 */
static void update_entries_visibility (void)
{
  short new_only = option (OPTSIDEBARNEWMAILONLY);
  SBENTRY *sbe;
  int i;

  for (i = 0; i < EntryCount; i++)
  {
    sbe = Entries[i];

    sbe->is_hidden = 0;

    if (!new_only)
      continue;

    if ((i == OpnIndex) || (sbe->buffy->msg_unread  > 0) || sbe->buffy->new ||
        (sbe->buffy->msg_flagged > 0))
      continue;

    if (Context && (mutt_strcmp (sbe->buffy->realpath, Context->realpath) == 0))
      /* Spool directory */
      continue;

    if (mutt_find_list (SidebarWhitelist, mutt_b2s (sbe->buffy->pathbuf)))
      /* Explicitly asked to be visible */
      continue;

    sbe->is_hidden = 1;
  }
}

/**
 * unsort_entries - Restore Entries array order to match Buffy list order
 */
static void unsort_entries (void)
{
  BUFFY *cur = Incoming;
  int i = 0, j;
  SBENTRY *tmp;

  while (cur && (i < EntryCount))
  {
    j = i;
    while ((j < EntryCount) &&
           (Entries[j]->buffy != cur))
      j++;
    if (j < EntryCount)
    {
      if (j != i)
      {
        tmp = Entries[i];
        Entries[i] = Entries[j];
        Entries[j] = tmp;
      }
      i++;
    }
    cur = cur->next;
  }
}

/**
 * sort_entries - Sort Entries array.
 *
 * Sort the Entries array according to the current sort config
 * option "sidebar_sort_method". This calls qsort to do the work which calls our
 * callback function "cb_qsort_sbe".
 *
 * Once sorted, the prev/next links will be reconstructed.
 */
static void sort_entries (void)
{
  short ssm = (SidebarSortMethod & SORT_MASK);

  /* These are the only sort methods we understand */
  if ((ssm == SORT_COUNT)     ||
      (ssm == SORT_UNREAD)    ||
      (ssm == SORT_FLAGGED)   ||
      (ssm == SORT_PATH))
    qsort (Entries, EntryCount, sizeof (*Entries), cb_qsort_sbe);
  else if ((ssm == SORT_ORDER) &&
           (SidebarSortMethod != PreviousSort))
    unsort_entries ();
}

/**
 * prepare_sidebar - Prepare the list of SBENTRYs for the sidebar display
 * @page_size:  The number of lines on a page
 *
 * Before painting the sidebar, we determine which are visible, sort
 * them and set up our page pointers.
 *
 * This is a lot of work to do each refresh, but there are many things that
 * can change outside of the sidebar that we don't hear about.
 *
 * Returns:
 *	0: No, don't draw the sidebar
 *	1: Yes, draw the sidebar
 */
static int prepare_sidebar (int page_size)
{
  int i;
  SBENTRY *opn_entry = NULL, *hil_entry = NULL;
  int page_entries;

  if (!EntryCount || (page_size <= 0))
    return 0;

  if (OpnIndex >= 0)
    opn_entry = Entries[OpnIndex];
  if (HilIndex >= 0)
    hil_entry = Entries[HilIndex];

  update_entries_visibility ();
  sort_entries ();

  for (i = 0; i < EntryCount; i++)
  {
    if (opn_entry == Entries[i])
      OpnIndex = i;
    if (hil_entry == Entries[i])
      HilIndex = i;
  }

  if ((HilIndex < 0) || Entries[HilIndex]->is_hidden ||
      (SidebarSortMethod != PreviousSort))
  {
    if (OpnIndex >= 0)
      HilIndex = OpnIndex;
    else
    {
      HilIndex = 0;
      if (Entries[HilIndex]->is_hidden)
        select_next ();
    }
  }

  /* Set the Top and Bottom to frame the HilIndex in groups of page_size */

  /* If OPTSIDEBARNEMAILONLY is set, some entries may be hidden so we
   * need to scan for the framing interval */
  if (option (OPTSIDEBARNEWMAILONLY))
  {
    TopIndex = BotIndex = -1;
    while (BotIndex < HilIndex)
    {
      TopIndex = BotIndex + 1;
      page_entries = 0;
      while (page_entries < page_size)
      {
        BotIndex++;
        if (BotIndex >= EntryCount)
          break;
        if (! Entries[BotIndex]->is_hidden)
          page_entries++;
      }
    }
  }
  /* Otherwise we can just calculate the interval */
  else
  {
    TopIndex = (HilIndex / page_size) * page_size;
    BotIndex = TopIndex + page_size - 1;
  }

  if (BotIndex > (EntryCount - 1))
    BotIndex = EntryCount - 1;

  PreviousSort = SidebarSortMethod;
  return 1;
}

/**
 * draw_divider - Draw a line between the sidebar and the rest of mutt
 * @num_rows:   Height of the Sidebar
 * @num_cols:   Width of the Sidebar
 *
 * Draw a divider using characters from the config option "sidebar_divider_char".
 * This can be an ASCII or Unicode character.  First we calculate this
 * characters' width in screen columns, then subtract that from the config
 * option "sidebar_width".
 *
 * Returns:
 *	-1: Error: bad character, etc
 *	0:  Error: 0 width character
 *	n:  Success: character occupies n screen columns
 */
static int draw_divider (int num_rows, int num_cols)
{
  /* Calculate the width of the delimiter in screen cells */
  int delim_len = mutt_strwidth (SidebarDividerChar);

  if (delim_len < 1)
    return delim_len;

  if (delim_len > num_cols)
    return 0;

  SETCOLOR(MT_COLOR_DIVIDER);

  int i;
  for (i = 0; i < num_rows; i++)
  {
    mutt_window_move (MuttSidebarWindow, i, SidebarWidth - delim_len);	//RAR 0 for rhs
    addstr (NONULL(SidebarDividerChar));
  }

  return delim_len;
}

/**
 * fill_empty_space - Wipe the remaining Sidebar space
 * @first_row:  Window line to start (0-based)
 * @num_rows:   Number of rows to fill
 * @width:      Width of the Sidebar (minus the divider)
 *
 * Write spaces over the area the sidebar isn't using.
 */
static void fill_empty_space (int first_row, int num_rows, int width)
{
  /* Fill the remaining rows with blank space */
  SETCOLOR(MT_COLOR_NORMAL);

  int r;
  for (r = 0; r < num_rows; r++)
  {
    mutt_window_move (MuttSidebarWindow, first_row + r, 0);	//RAR rhs
    int i;
    for (i = 0; i < width; i++)
      addch (' ');
  }
}

/**
 * draw_sidebar - Write out a list of mailboxes, on the left
 * @num_rows:   Height of the Sidebar
 * @num_cols:   Width of the Sidebar
 * @div_width:  Width in screen characters taken by the divider
 *
 * Display a list of mailboxes in a panel on the left.  What's displayed will
 * depend on our index markers: TopBuffy, OpnBuffy, HilBuffy, BotBuffy.
 * On the first run they'll be NULL, so we display the top of Mutt's list
 * (Incoming).
 *
 * TopBuffy - first visible mailbox
 * BotBuffy - last  visible mailbox
 * OpnBuffy - mailbox shown in Mutt's Index Panel
 * HilBuffy - Unselected mailbox (the paging follows this)
 *
 * The entries are formatted using "sidebar_format" and may be abbreviated:
 * "sidebar_short_path", indented: "sidebar_folder_indent",
 * "sidebar_indent_string" and sorted: "sidebar_sort_method".  Finally, they're
 * trimmed to fit the available space.
 */
static void draw_sidebar (int num_rows, int num_cols, int div_width)
{
  struct { short depth, width; } stack[32];
  int stack_index = 0;
  stack[0].depth = 0;
  stack[0].width = -1;
  const char *last_folder_name = NULL;
  int entryidx;
  SBENTRY *entry;
  BUFFY *b;
  if (TopIndex < 0)
    return;

  int w = MIN(num_cols, (SidebarWidth - div_width));
  int row = 0;
  for (entryidx = TopIndex; (entryidx < EntryCount) && (row < num_rows); entryidx++)
  {
    entry = Entries[entryidx];
    if (entry->is_hidden)
      continue;
    b = entry->buffy;

    if (entryidx == OpnIndex)
    {
      if ((ColorDefs[MT_COLOR_SB_INDICATOR] != 0))
        SETCOLOR(MT_COLOR_SB_INDICATOR);
      else
        SETCOLOR(MT_COLOR_INDICATOR);
    }
    else if (entryidx == HilIndex)
      SETCOLOR(MT_COLOR_HIGHLIGHT);
    else if ((b->msg_unread > 0) || (b->new))
      SETCOLOR(MT_COLOR_NEW);
    else if (b->msg_flagged > 0)
      SETCOLOR(MT_COLOR_FLAGGED);
    else if ((ColorDefs[MT_COLOR_SB_SPOOLFILE] != 0) &&
             (mutt_strcmp (mutt_b2s (b->pathbuf), Spoolfile) == 0))
      SETCOLOR(MT_COLOR_SB_SPOOLFILE);
    else
      SETCOLOR(MT_COLOR_NORMAL);

    mutt_window_move (MuttSidebarWindow, row, 0);
    if (Context && Context->realpath &&
        !mutt_strcmp (b->realpath, Context->realpath))
    {
      b->msg_unread  = Context->unread;
      b->msg_count   = Context->msgcount;
      b->msg_flagged = Context->flagged;
    }

    /* compute length of Maildir without trailing separator */
    size_t maildirlen = mutt_strlen (Maildir);
    if (maildirlen &&
        SidebarDelimChars &&
        strchr (SidebarDelimChars, Maildir[maildirlen - 1]))
      maildirlen--;

    /* check whether Maildir is a prefix of the current folder's path */
    short maildir_is_prefix = 0;
    if ((mutt_buffer_len (b->pathbuf) > maildirlen) &&
        (mutt_strncmp (Maildir, mutt_b2s (b->pathbuf), maildirlen) == 0) &&
        SidebarDelimChars &&
        strchr (SidebarDelimChars, mutt_b2s (b->pathbuf)[maildirlen]))
      maildir_is_prefix = 1;

    const char *sidebar_folder_name =
      mutt_b2s (b->pathbuf) + maildir_is_prefix * (maildirlen + 1);
    BUFFER *short_folder_name = mutt_buffer_pool_get ();

    if (option (OPTSIDEBARSHORTPATH) || option (OPTSIDEBARFOLDERINDENT))
    {
      int common_depth = 0, depth = 0, i = 0;
      while (1)
      {
	if (/* delimiter or terminating null in current folder ? */
	    (sidebar_folder_name[i] == '\0' ||
	     (SidebarDelimChars &&
	     strchr (SidebarDelimChars, sidebar_folder_name[i]))))
	{
	  depth++;
	  if (/* matching delimiter or null in previous folderh ? */
	      last_folder_name &&
	      (last_folder_name[i] == '\0' ||
	       last_folder_name[i] == sidebar_folder_name[i]))
	  {
	    common_depth++;
	    if (last_folder_name[i] == '\0') last_folder_name = NULL;
	  }
	  /* immediately break on terminating delimiter, don't wait for null
	   * (like in "Mail/inbox/") */
	  if (sidebar_folder_name[i] == '\0' ||
	      sidebar_folder_name[i+1] == '\0')
	    break;
	}
	if (last_folder_name &&
	    last_folder_name[i] != sidebar_folder_name[i])
	  last_folder_name = NULL;
	i++;
      }
      last_folder_name = sidebar_folder_name;

      while (stack[stack_index].depth > common_depth) stack_index--;
      int indent_depth = stack[stack_index].depth;
      int indent_width = stack[stack_index].width;
      if (depth > indent_depth) indent_width++;
      if (stack_index + 1 < sizeof(stack)) stack_index++;
      stack[stack_index].depth = depth;
      stack[stack_index].width = indent_width;

      if (option (OPTSIDEBARSHORTPATH) && indent_depth > 0) {
	do
	{
	  while (--i >= 0 &&
		 ! strchr (SidebarDelimChars, sidebar_folder_name[i]));
	  depth--;
	} while (depth > indent_depth);
	sidebar_folder_name += i + 1;
	if (depth > 0) maildir_is_prefix = 0;
      }

      if (option (OPTSIDEBARFOLDERINDENT))
      {
	for (i=0; i < indent_width; i++)
	  mutt_buffer_addstr (short_folder_name, NONULL(SidebarIndentString));
      }
    }
    if (maildir_is_prefix)
      mutt_buffer_addstr (short_folder_name, "+");
    mutt_buffer_addstr (short_folder_name, sidebar_folder_name);
    sidebar_folder_name = mutt_b2s (short_folder_name);

    char str[STRING];
    make_sidebar_entry (str, sizeof (str), w, sidebar_folder_name, entry);
    printw ("%s", str);
    mutt_buffer_pool_release (&short_folder_name);
    row++;
  }

  fill_empty_space (row, num_rows - row, w);
}


/**
 * mutt_sb_draw - Completely redraw the sidebar
 *
 * Completely refresh the sidebar region.  First draw the divider; then, for
 * each BUFFY, call make_sidebar_entry; finally blank out any remaining space.
 */
void mutt_sb_draw (void)
{
  if (!option (OPTSIDEBAR))
    return;

  int num_rows  = MuttSidebarWindow->rows;
  int num_cols  = MuttSidebarWindow->cols;

  int div_width = draw_divider (num_rows, num_cols);
  if (div_width < 0)
    return;

  if (!Incoming)
  {
    fill_empty_space (0, num_rows, SidebarWidth - div_width);
    return;
  }

  if (!prepare_sidebar (num_rows))
    return;

  draw_sidebar (num_rows, num_cols, div_width);
}

/**
 * select_next - Selects the next unhidden mailbox
 *
 * Returns:
 *      1: Success
 *      0: Failure
 */
static int select_next (void)
{
  int entry = HilIndex;

  if (!EntryCount || HilIndex < 0)
    return 0;

  do
  {
    entry++;
    if (entry == EntryCount)
      return 0;
  } while (Entries[entry]->is_hidden);

  HilIndex = entry;
  return 1;
}

/**
 * select_next_new - Selects the next new mailbox
 *
 * Search down the list of mail folders for one containing new mail.
 *
 * Returns:
 *	1: Success
 *	0: Failure
 */
static int select_next_new (void)
{
  int entry = HilIndex;

  if (!EntryCount || HilIndex < 0)
    return 0;

  do
  {
    entry++;
    if (entry == EntryCount)
    {
      if (option (OPTSIDEBARNEXTNEWWRAP))
        entry = 0;
      else
        return 0;
    }
    if (entry == HilIndex)
      return 0;
  } while (!Entries[entry]->buffy->new &&
           !Entries[entry]->buffy->msg_unread);

  HilIndex = entry;
  return 1;
}

/**
 * select_prev - Selects the previous unhidden mailbox
 *
 * Returns:
 *      1: Success
 *      0: Failure
 */
static int select_prev (void)
{
  int entry = HilIndex;

  if (!EntryCount || HilIndex < 0)
    return 0;

  do
  {
    entry--;
    if (entry < 0)
      return 0;
  } while (Entries[entry]->is_hidden);

  HilIndex = entry;
  return 1;
}

/**
 * select_prev_new - Selects the previous new mailbox
 *
 * Search up the list of mail folders for one containing new mail.
 *
 * Returns:
 *	1: Success
 *	0: Failure
 */
static int select_prev_new (void)
{
  int entry = HilIndex;

  if (!EntryCount || HilIndex < 0)
    return 0;

  do
  {
    entry--;
    if (entry < 0)
    {
      if (option (OPTSIDEBARNEXTNEWWRAP))
        entry = EntryCount - 1;
      else
        return 0;
    }
    if (entry == HilIndex)
      return 0;
  } while (!Entries[entry]->buffy->new &&
           !Entries[entry]->buffy->msg_unread);

  HilIndex = entry;
  return 1;
}

/**
 * select_page_down - Selects the first entry in the next page of mailboxes
 *
 * Returns:
 *      1: Success
 *      0: Failure
 */
static int select_page_down (void)
{
  int orig_hil_index = HilIndex;

  if (!EntryCount || BotIndex < 0)
    return 0;

  HilIndex = BotIndex;
  select_next ();
  /* If the rest of the entries are hidden, go up to the last unhidden one */
  if (Entries[HilIndex]->is_hidden)
    select_prev ();

  return (orig_hil_index != HilIndex);
}

/**
 * select_page_up - Selects the last entry in the previous page of mailboxes
 *
 * Returns:
 *      1: Success
 *      0: Failure
 */
static int select_page_up (void)
{
  int orig_hil_index = HilIndex;

  if (!EntryCount || TopIndex < 0)
    return 0;

  HilIndex = TopIndex;
  select_prev ();
  /* If the rest of the entries are hidden, go down to the last unhidden one */
  if (Entries[HilIndex]->is_hidden)
    select_next ();

  return (orig_hil_index != HilIndex);
}

/**
 * mutt_sb_change_mailbox - Change the selected mailbox
 * @op: Operation code
 *
 * Change the selected mailbox, e.g. "Next mailbox", "Previous Mailbox
 * with new mail". The operations are listed OPS.SIDEBAR which is built
 * into an enum in keymap_defs.h.
 *
 * If the operation is successful, HilBuffy will be set to the new mailbox.
 * This function only *selects* the mailbox, doesn't *open* it.
 *
 * Allowed values are: OP_SIDEBAR_NEXT, OP_SIDEBAR_NEXT_NEW,
 * OP_SIDEBAR_PAGE_DOWN, OP_SIDEBAR_PAGE_UP, OP_SIDEBAR_PREV,
 * OP_SIDEBAR_PREV_NEW.
 */
void mutt_sb_change_mailbox (int op)
{
  if (!option (OPTSIDEBAR))
    return;

  if (HilIndex < 0)	/* It'll get reset on the next draw */
    return;

  switch (op)
  {
    case OP_SIDEBAR_NEXT:
      if (! select_next ())
        return;
      break;
    case OP_SIDEBAR_NEXT_NEW:
      if (! select_next_new ())
        return;
      break;
    case OP_SIDEBAR_PAGE_DOWN:
      if (! select_page_down ())
        return;
      break;
    case OP_SIDEBAR_PAGE_UP:
      if (! select_page_up ())
        return;
      break;
    case OP_SIDEBAR_PREV:
      if (! select_prev ())
        return;
      break;
    case OP_SIDEBAR_PREV_NEW:
      if (! select_prev_new ())
        return;
      break;
    default:
      return;
  }
  mutt_set_current_menu_redraw (REDRAW_SIDEBAR);
}

/**
 * mutt_sb_set_buffystats - Update the BUFFY's message counts from the CONTEXT
 * @ctx:  A mailbox CONTEXT
 *
 * Given a mailbox CONTEXT, find a matching mailbox BUFFY and copy the message
 * counts into it.
 */
void mutt_sb_set_buffystats (const CONTEXT *ctx)
{
  /* Even if the sidebar's hidden,
   * we should take note of the new data. */
  BUFFY *b = Incoming;
  if (!ctx || !b)
    return;

  for (; b; b = b->next)
  {
    if (!mutt_strcmp (b->realpath, ctx->realpath))
    {
      b->msg_unread  = ctx->unread;
      b->msg_count   = ctx->msgcount;
      b->msg_flagged = ctx->flagged;
      break;
    }
  }
}

/**
 * mutt_sb_get_highlight - Get the BUFFY that's highlighted in the sidebar
 *
 * Get the path of the mailbox that's highlighted in the sidebar.
 *
 * Returns:
 *	Mailbox path
 */
const char *mutt_sb_get_highlight (void)
{
  if (!option (OPTSIDEBAR))
    return NULL;

  if (!EntryCount || HilIndex < 0)
    return NULL;

  return mutt_b2s (Entries[HilIndex]->buffy->pathbuf);
}

/**
 * mutt_sb_set_open_buffy - Set the OpnBuffy based on the global Context
 *
 * Search through the list of mailboxes.  If a BUFFY has a matching path, set
 * OpnBuffy to it.
 */
void mutt_sb_set_open_buffy (void)
{
  int entry;

  OpnIndex = -1;

  if (!Context)
    return;

  for (entry = 0; entry < EntryCount; entry++)
  {
    if (!mutt_strcmp (Entries[entry]->buffy->realpath, Context->realpath))
    {
      OpnIndex = entry;
      HilIndex = entry;
      break;
    }
  }
}

/**
 * mutt_sb_notify_mailbox - The state of a BUFFY is about to change
 *
 * We receive a notification:
 *	After a new BUFFY has been created
 *	Before a BUFFY is deleted
 *
 * Before a deletion, check that our pointers won't be invalidated.
 */
void mutt_sb_notify_mailbox (BUFFY *b, int created)
{
  int del_index;

  if (!b)
    return;

  /* Any new/deleted mailboxes will cause a refresh.  As long as
   * they're valid, our pointers will be updated in prepare_sidebar() */

  if (created)
  {
    if (EntryCount >= EntryLen)
    {
      EntryLen += 10;
      safe_realloc (&Entries, EntryLen * sizeof (SBENTRY *));
    }
    Entries[EntryCount] = safe_calloc (1, sizeof(SBENTRY));
    Entries[EntryCount]->buffy = b;

    if (TopIndex < 0)
      TopIndex = EntryCount;
    if (BotIndex < 0)
      BotIndex = EntryCount;
    if ((OpnIndex < 0) && Context &&
        (mutt_strcmp (b->realpath, Context->realpath) == 0))
      OpnIndex = EntryCount;

    EntryCount++;
  }
  else
  {
    for (del_index = 0; del_index < EntryCount; del_index++)
      if (Entries[del_index]->buffy == b)
        break;
    if (del_index == EntryCount)
      return;
    FREE (&Entries[del_index]);
    EntryCount--;

    if (TopIndex > del_index || TopIndex == EntryCount)
      TopIndex--;
    if (OpnIndex == del_index)
      OpnIndex = -1;
    else if (OpnIndex > del_index)
      OpnIndex--;
    if (HilIndex > del_index || HilIndex == EntryCount)
      HilIndex--;
    if (BotIndex > del_index || BotIndex == EntryCount)
      BotIndex--;

    for (; del_index < EntryCount; del_index++)
      Entries[del_index] = Entries[del_index + 1];
  }

  mutt_set_current_menu_redraw (REDRAW_SIDEBAR);
}
