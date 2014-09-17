/* gb-source-auto-indenter-c.c
 *
 * Copyright (C) 2014 Christian Hergert <christian@hergert.me>
 *
 * This file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "indent"

#include <glib/gi18n.h>

#include "gb-log.h"
#include "gb-source-auto-indenter-c.h"

struct _GbSourceAutoIndenterCPrivate
{
  gint scope_indent;     /* after { */
  gint condition_indent; /* for, if, while, switch, etc */
};

enum
{
  PROP_0,
  PROP_SCOPE_INDENT,
  LAST_PROP
};

G_DEFINE_TYPE_WITH_PRIVATE (GbSourceAutoIndenterC, gb_source_auto_indenter_c,
                            GB_TYPE_SOURCE_AUTO_INDENTER)

static GParamSpec *gParamSpecs [LAST_PROP];

GbSourceAutoIndenter *
gb_source_auto_indenter_c_new (void)
{
  return g_object_new (GB_TYPE_SOURCE_AUTO_INDENTER_C, NULL);
}

static inline void
build_indent (GbSourceAutoIndenterC *c,
              guint                  line_offset,
              GtkTextIter           *matching_line,
              GString               *str)
{
  GtkTextIter iter;
  gunichar ch;

  gtk_text_buffer_get_iter_at_line (gtk_text_iter_get_buffer (matching_line),
                                    &iter,
                                    gtk_text_iter_get_line (matching_line));

  do {
    ch = gtk_text_iter_get_char (&iter);

    switch (ch) {
    case '\t':
    case ' ':
      g_string_append_unichar (str, ch);
      break;
    default:
      g_string_append_c (str, ' ');
      break;
    }
  } while (gtk_text_iter_forward_char (&iter) &&
           gtk_text_iter_compare (&iter, matching_line) <= 0);

  while (str->len < line_offset)
    g_string_append_c (str, ' ');
}

static gboolean
backward_find_matching_char (GtkTextIter *iter,
                             gunichar     ch)
{
  gunichar match;
  gunichar cur;
  guint count = 1;

  switch (ch) {
  case ')':
    match = '(';
    break;
  case '}':
    match = '{';
    break;
  case '[':
    match = ']';
    break;
  default:
    g_assert_not_reached ();
    break;
  }

  while (gtk_text_iter_backward_char (iter))
    {
      cur = gtk_text_iter_get_char (iter);

      if (cur == match)
        {
          if (--count == 0)
            return TRUE;
        }
      else if (cur == ch)
        count++;
    }

  return FALSE;
}

static gboolean
line_is_space (GtkTextIter *iter)
{
  GtkTextIter begin;

  gtk_text_buffer_get_iter_at_line (gtk_text_iter_get_buffer (iter),
                                    &begin,
                                    gtk_text_iter_get_line (iter));

  while (!gtk_text_iter_equal (iter, &begin))
    {
      gunichar ch;

      if (!gtk_text_iter_forward_char (&begin))
        break;

      ch = gtk_text_iter_get_char (&begin);

      switch (ch) {
      case '\t':
      case ' ':
        break;
      default:
        return FALSE;
      }
    }

  return TRUE;
}

static gboolean
backward_find_stmt_expr (GtkTextIter *iter)
{
  return FALSE;
}

static gchar *
gb_source_auto_indenter_c_query (GbSourceAutoIndenter *indenter,
                                 GtkTextView          *view,
                                 GtkTextBuffer        *buffer,
                                 GtkTextIter          *iter)
{
  GbSourceAutoIndenterCPrivate *priv;
  GbSourceAutoIndenterC *c = (GbSourceAutoIndenterC *)indenter;
  GtkTextIter cur;
  gunichar ch;
  GString *str;
  gchar *ret;

  ENTRY;

  g_return_val_if_fail (GB_IS_SOURCE_AUTO_INDENTER_C (c), NULL);

  priv = c->priv;

  /*
   * Save our current iter position to restore it later.
   */
  gtk_text_iter_assign (&cur, iter);

  /*
   * Create the buffer for our indentation string.
   */
  str = g_string_new (NULL);

  /*
   * Move back to the character before the \n that was inserted.
   *
   * TODO: This assumption may change.
   */
  if (!gtk_text_iter_backward_char (iter))
    GOTO (cleanup);

  /*
   * Get our last non \n character entered.
   */
  ch = gtk_text_iter_get_char (iter);

  /*
   * We just started a new scope. Try to find the indentation of the previous
   * scope and our indentation past it.
   */
  if (ch == '{')
    {
      if (line_is_space (iter))
        {
          guint offset;

          offset = gtk_text_iter_get_line_offset (iter);
          build_indent (c, offset + priv->scope_indent, iter, str);
          GOTO (cleanup);
        }
    }

  /*
   * If we just placed a terminating parenthesis, we need to work our way back
   * to it's match. That way we can peak at what it was and determine
   * indentation from that.
   */
  if (ch == ')' || ch == ']' || ch == '}')
    {
      if (!backward_find_matching_char (iter, ch))
        GOTO (cleanup);
    }

  /*
   * We are probably in a a function call or parameter list.  Let's try to work
   * our way back to the opening parenthesis. This should work when the target
   * is for, parameter lists, or function arguments.
   */
  if (ch == ',')
    {
      if (!backward_find_matching_char (iter, ')'))
        GOTO (cleanup);

      build_indent (c, gtk_text_iter_get_line_offset (iter), iter, str);
      GOTO (cleanup);
    }

  /*
   * Looks like the last line was a statement or expression. Let's try to
   * find the beginning of it.
   */
  if (ch == ';')
    {
      guint offset;

      if (!backward_find_stmt_expr (iter))
        {
          gtk_text_iter_assign (iter, &cur);
          build_indent (c, 0, iter, str);
        }
      else
        {
          offset = gtk_text_iter_get_line_offset (iter);
          build_indent (c, offset, iter, str);
        }

      GOTO (cleanup);
    }

cleanup:
  gtk_text_iter_assign (iter, &cur);

  ret = g_string_free (str, FALSE);

  RETURN (ret);
}

static void
gb_source_auto_indenter_c_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  GbSourceAutoIndenterC *c = GB_SOURCE_AUTO_INDENTER_C (object);

  switch (prop_id) {
  case PROP_SCOPE_INDENT:
    g_value_set_uint (value, c->priv->scope_indent);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gb_source_auto_indenter_c_set_property (GObject      *object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  GbSourceAutoIndenterC *c = GB_SOURCE_AUTO_INDENTER_C (object);

  switch (prop_id) {
  case PROP_SCOPE_INDENT:
    c->priv->scope_indent = g_value_get_uint (value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }

  g_object_notify_by_pspec (object, pspec);
}

static void
gb_source_auto_indenter_c_class_init (GbSourceAutoIndenterCClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GbSourceAutoIndenterClass *indenter_class = GB_SOURCE_AUTO_INDENTER_CLASS (klass);

  object_class->get_property = gb_source_auto_indenter_c_get_property;
  object_class->set_property = gb_source_auto_indenter_c_set_property;

  indenter_class->query = gb_source_auto_indenter_c_query;

  gParamSpecs [PROP_SCOPE_INDENT] =
    g_param_spec_int ("scope-indent",
                      _("Name"),
                      _("Name"),
                      -32,
                      32,
                      2,
                      (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SCOPE_INDENT,
                                   gParamSpecs [PROP_SCOPE_INDENT]);
}

static void
gb_source_auto_indenter_c_init (GbSourceAutoIndenterC *c)
{
  c->priv = gb_source_auto_indenter_c_get_instance_private (c);

  c->priv->condition_indent = 2;
  c->priv->scope_indent = 2;
}