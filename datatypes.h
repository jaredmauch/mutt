/*
 * Copyright (C) 1996-2002,2007,2010,2012-2013,2016 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2004 g10 Code GmbH
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

#ifndef INCLUDED_MUTT_DATATYPES_H
#define INCLUDED_MUTT_DATATYPES_H

/* Data type definitions for muttrc variables. */
#define DT_MASK      0x0f
#define DT_NONE         0 /* well, nothing.. for makedoc.c */
#define DT_BOOL         1 /* boolean option */
#define DT_NUM          2 /* a number (short) */
#define DT_STR          3 /* a string */
#define DT_PATH         4 /* a pathname */
#define DT_QUAD         5 /* quad-option (yes/no/ask-yes/ask-no) */
#define DT_SORT         6 /* sorting methods */
#define DT_RX           7 /* regular expressions */
#define DT_MAGIC        8 /* mailbox type */
#define DT_SYN          9 /* synonym for another variable */
#define DT_ADDR        10 /* e-mail address */
#define DT_MBCHARTBL   11 /* multibyte char table */
#define DT_LNUM        12 /* a number (long) */

#endif /* INCLUDED_MUTT_DATATYPES_H */
