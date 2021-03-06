/* gb-workbench-types.h
 *
 * Copyright (C) 2014 Christian Hergert <christian@hergert.me>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GB_WORKBENCH_TYPES_H
#define GB_WORKBENCH_TYPES_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _GbWorkbench        GbWorkbench;
typedef struct _GbWorkbenchClass   GbWorkbenchClass;
typedef struct _GbWorkbenchPrivate GbWorkbenchPrivate;

typedef struct _GbWorkspace        GbWorkspace;
typedef struct _GbWorkspaceClass   GbWorkspaceClass;
typedef struct _GbWorkspacePrivate GbWorkspacePrivate;

G_END_DECLS

#endif /* GB_WORKBENCH_TYPES_H */
