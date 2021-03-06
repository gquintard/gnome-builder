/* gb-editor-document.c
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

#define G_LOG_DOMAIN "editor-document"

#include <glib/gi18n.h>
#include <gtksourceview/gtksource.h>

#include "gb-document.h"
#include "gb-doc-seq.h"
#include "gb-editor-document.h"
#include "gb-editor-file-marks.h"
#include "gb-editor-view.h"
#include "gb-log.h"
#include "gb-gtk.h"
#include "gca-structs.h"

struct _GbEditorDocumentPrivate
{
  GtkSourceFile         *file;
  GbSourceChangeMonitor *change_monitor;
  GbSourceCodeAssistant *code_assistant;
  gchar                 *title;
  GCancellable          *cancellable;
  GError                *error;

  gdouble                progress;
  guint                  doc_seq_id;
  GTimeVal               mtime;
  GTimeVal               unsaved_ctime;

  guint                  file_changed_on_volume : 1;
  guint                  mtime_set : 1;
  guint                  read_only : 1;
  guint                  trim_trailing_whitespace : 1;
};

enum {
  PROP_0,
  PROP_CHANGE_MONITOR,
  PROP_ERROR,
  PROP_FILE,
  PROP_FILE_CHANGED_ON_VOLUME,
  PROP_MODIFIED,
  PROP_PROGRESS,
  PROP_READ_ONLY,
  PROP_STYLE_SCHEME_NAME,
  PROP_TITLE,
  PROP_TRIM_TRAILING_WHITESPACE,
  LAST_PROP
};

enum {
  CURSOR_MOVED,
  FILE_MARK_SET,
  SAVED,
  LAST_SIGNAL
};

static void gb_editor_document_init_document (GbDocumentInterface *iface);
static void gb_editor_document_update_title  (GbEditorDocument *document);

G_DEFINE_TYPE_EXTENDED (GbEditorDocument,
                        gb_editor_document,
                        GTK_SOURCE_TYPE_BUFFER,
                        0,
                        G_ADD_PRIVATE (GbEditorDocument)
                        G_IMPLEMENT_INTERFACE (GB_TYPE_DOCUMENT,
                                               gb_editor_document_init_document))

static GParamSpec *gParamSpecs [LAST_PROP];
static guint gSignals [LAST_SIGNAL];

GbEditorDocument *
gb_editor_document_new (void)
{
  return g_object_new (GB_TYPE_EDITOR_DOCUMENT, NULL);
}

static gboolean
gb_editor_document_is_untitled (GbDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);

  return (GB_EDITOR_DOCUMENT (document)->priv->doc_seq_id > 0);
}

/**
 * gb_editor_document_get_error:
 *
 * Fetches the most recent error for the #GbEditorDocument instance. If no
 * error has been registered, %NULL is returned.
 *
 * Returns: (transfer none): A #GError or %NULL.
 */
const GError *
gb_editor_document_get_error (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  return document->priv->error;
}

static void
gb_editor_document_set_error (GbEditorDocument *document,
                              const GError     *error)
{
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  if (error != document->priv->error)
    {
      g_clear_error (&document->priv->error);
      document->priv->error = g_error_copy (error);
      g_object_notify_by_pspec (G_OBJECT (document), gParamSpecs [PROP_ERROR]);
    }
}

static gboolean
gb_editor_document_get_read_only (GbDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);

  return GB_EDITOR_DOCUMENT (document)->priv->read_only;
}

static void
gb_editor_document_set_read_only (GbEditorDocument *document,
                                  gboolean          read_only)
{
  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  if (document->priv->read_only != read_only)
    {
      document->priv->read_only = read_only;
      g_object_notify (G_OBJECT (document), "read-only");
      gb_editor_document_update_title (document);
    }

  EXIT;
}

gboolean
gb_editor_document_get_file_changed_on_volume (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);

  return document->priv->file_changed_on_volume;
}

static void
gb_editor_document_set_file_changed_on_volume (GbEditorDocument *document,
                                               gboolean          file_changed_on_volume)
{
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  if (file_changed_on_volume != document->priv->file_changed_on_volume)
    {
      document->priv->file_changed_on_volume = file_changed_on_volume;
      g_object_notify_by_pspec (G_OBJECT (document),
                                gParamSpecs [PROP_FILE_CHANGED_ON_VOLUME]);
    }
}

static void
gb_editor_document_check_modified_cb (GObject      *object,
                                      GAsyncResult *result,
                                      gpointer      user_data)
{
  GbEditorDocument *document = user_data;
  GFileInfo *info;
  GError *error = NULL;
  GFile *file = (GFile *)object;

  g_return_if_fail (G_IS_FILE (file));
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  info = g_file_query_info_finish (file, result, &error);

  if (info)
    {
      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
        {
          gboolean read_only;

          read_only = !g_file_info_get_attribute_boolean (info,
                                                          G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
          if (gb_editor_document_get_read_only (GB_DOCUMENT (document)) != read_only)
            gb_editor_document_set_read_only (document, read_only);
        }

      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) && document->priv->mtime_set)
        {
          GTimeVal tv;

          g_file_info_get_modification_time (info, &tv);

          if (memcmp (&tv, &document->priv->mtime, sizeof tv) != 0)
            gb_editor_document_set_file_changed_on_volume (document, TRUE);
        }
    }

  g_clear_object (&document);
  g_clear_object (&info);
}

void
gb_editor_document_check_externally_modified (GbEditorDocument *document)
{
  GFile *location;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  if (document->priv->file_changed_on_volume)
    return;

  location = gtk_source_file_get_location (document->priv->file);
  if (!location)
    return;

  g_file_query_info_async (location,
                           G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                           G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                           G_FILE_QUERY_INFO_NONE,
                           G_PRIORITY_DEFAULT,
                           document->priv->cancellable,
                           gb_editor_document_check_modified_cb,
                           g_object_ref (document));
}

gdouble
gb_editor_document_get_progress (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), 0.0);

  return document->priv->progress;
}

static void
gb_editor_document_set_progress (GbEditorDocument *document,
                                 gdouble           progress)
{
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (progress >= 0.0);
  g_return_if_fail (progress <= 1.0);

  document->priv->progress = progress;
  g_object_notify_by_pspec (G_OBJECT (document), gParamSpecs [PROP_PROGRESS]);
}

gboolean
gb_editor_document_get_trim_trailing_whitespace (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);

  return document->priv->trim_trailing_whitespace;
}

void
gb_editor_document_set_trim_trailing_whitespace (GbEditorDocument *document,
                                                 gboolean          trim_trailing_whitespace)
{
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  if (trim_trailing_whitespace != document->priv->trim_trailing_whitespace)
    {
      document->priv->trim_trailing_whitespace = trim_trailing_whitespace;
      g_object_notify_by_pspec (G_OBJECT (document),
                                gParamSpecs [PROP_TRIM_TRAILING_WHITESPACE]);
    }
}

GbSourceChangeMonitor *
gb_editor_document_get_change_monitor (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  return document->priv->change_monitor;
}

GbSourceCodeAssistant *
gb_editor_document_get_code_assistant (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  return document->priv->code_assistant;
}

GtkSourceFile *
gb_editor_document_get_file (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  return document->priv->file;
}

static void
gb_editor_document_set_style_scheme_name (GbEditorDocument *document,
                                          const gchar      *style_scheme_name)
{
  GtkSourceStyleSchemeManager *manager;
  GtkSourceStyleScheme *scheme;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  manager = gtk_source_style_scheme_manager_get_default ();
  scheme = gtk_source_style_scheme_manager_get_scheme (manager,
                                                       style_scheme_name);
  gtk_source_buffer_set_style_scheme (GTK_SOURCE_BUFFER (document), scheme);
}

static void
gb_editor_document_mark_set (GtkTextBuffer     *buffer,
                             const GtkTextIter *iter,
                             GtkTextMark       *mark)
{
  if (GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->mark_set)
    GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->mark_set (buffer, iter, mark);

  if (mark == gtk_text_buffer_get_insert (buffer))
    g_signal_emit (buffer, gSignals [CURSOR_MOVED], 0);
}

static void
gb_editor_document_changed (GtkTextBuffer *buffer)
{
  g_assert (GB_IS_EDITOR_DOCUMENT (buffer));

  g_signal_emit (buffer, gSignals [CURSOR_MOVED], 0);

  GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->changed (buffer);
}

static void
gb_editor_document_add_diagnostic (GbEditorDocument *document,
                                   GcaDiagnostic    *diag,
                                   GcaSourceRange   *range)
{
  GtkTextBuffer *buffer;
  GtkTextIter begin;
  GtkTextIter end;
  guint column;

  g_assert (GB_IS_EDITOR_DOCUMENT (document));
  g_assert (diag);
  g_assert (range);

  if (range->begin.line == -1 || range->end.line == -1)
    return;

  buffer = GTK_TEXT_BUFFER (document);

  gtk_text_buffer_get_iter_at_line (buffer, &begin, range->begin.line);
  for (column = range->begin.column; column; column--)
    if (gtk_text_iter_ends_line (&begin) || !gtk_text_iter_forward_char (&begin))
      break;

  gtk_text_buffer_get_iter_at_line (buffer, &end, range->end.line);
  for (column = range->end.column; column; column--)
    if (gtk_text_iter_ends_line (&end) || !gtk_text_iter_forward_char (&end))
      break;

  if (gtk_text_iter_equal (&begin, &end))
    gtk_text_iter_forward_to_line_end (&end);

  gtk_text_buffer_apply_tag_by_name (buffer, "ErrorTag", &begin, &end);
}

static void
apply_tag_style (GbEditorDocument *document,
                 GtkTextTag       *tag,
                 const gchar      *style_id)
{
  GtkSourceStyleScheme *scheme;
  GtkSourceStyle *style;
  gboolean background_set;
  gboolean bold_set;
  gboolean foreground_set;
  gboolean line_background_set;
  gchar *str;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  scheme = gtk_source_buffer_get_style_scheme (GTK_SOURCE_BUFFER (document));
  if (!scheme)
    return;

  style = gtk_source_style_scheme_get_style (scheme, style_id);
  if (!style)
    return;

  g_object_get (style,
                "background-set", &background_set,
                "bold-set", &bold_set,
                "foreground-set", &foreground_set,
                "line-background-set", &line_background_set,
                NULL);

  if (background_set)
    {
      g_object_get (style, "background", &str, NULL);
      g_object_set (tag, "background", str, NULL);
      g_free (str);
    }
  else
    g_object_set (tag, "background-set", FALSE, NULL);

  if (bold_set)
    {
      PangoWeight weight;
      gboolean bold;

      g_object_get (style, "bold", &bold, NULL);
      weight = bold ? PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_BOLD;
      g_object_set (tag, "weight", weight, NULL);
    }
  else
    g_object_set (tag, "weight-set", FALSE, NULL);

  if (foreground_set)
    {
      g_object_get (style, "foreground", &str, NULL);
      g_object_set (tag, "foreground", str, NULL);
      g_free (str);
    }
  else
    g_object_set (tag, "foreground-set", FALSE, NULL);

  if (line_background_set)
    {
      g_object_get (style, "line-background", &str, NULL);
      g_object_set (tag, "paragraph-background", str, NULL);
      g_free (str);
    }
  else
    g_object_set (tag, "paragraph-background-set", FALSE, NULL);
}

static GtkTextTag *
gb_editor_document_get_error_tag (GbEditorDocument *document)
{
  GtkTextBuffer *buffer;
  GtkTextTagTable *tag_table;
  GtkTextTag *tag;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  buffer = GTK_TEXT_BUFFER (document);
  tag_table = gtk_text_buffer_get_tag_table (buffer);
  tag = gtk_text_tag_table_lookup (tag_table, "ErrorTag");

  if (!tag)
    {
      tag = gtk_text_buffer_create_tag (buffer, "ErrorTag",
                                        "underline", PANGO_UNDERLINE_ERROR,
                                        NULL);
      apply_tag_style (document, tag, "def:error");
    }

  return tag;
}

static void
gb_editor_document_notify_style_scheme (GbEditorDocument *document,
                                        GParamSpec       *pspec,
                                        gpointer          unused)
{
  GtkTextTag *tag;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  tag = gb_editor_document_get_error_tag (document);
  apply_tag_style (document, tag, "def:error");
}

static void
gb_editor_document_code_assistant_changed (GbEditorDocument      *document,
                                           GbSourceCodeAssistant *code_assistant)
{
  GtkTextIter begin;
  GtkTextIter end;
  GtkTextTag *tag;
  GArray *ar;
  guint i;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (code_assistant));

  /*
   * Update all of the error tags in the buffer based on the diagnostics
   * returned from code assistance. We might want to find a way to do this
   * iteratively in the background based interactivity.
   */

  tag = gb_editor_document_get_error_tag (document);

  gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (document), &begin, &end);
  gtk_text_buffer_remove_tag (GTK_TEXT_BUFFER (document), tag, &begin, &end);

  ar = gb_source_code_assistant_get_diagnostics (code_assistant);

  for (i = 0; i < ar->len; i++)
    {
      GcaDiagnostic *diag;
      guint j;

      diag = &g_array_index (ar, GcaDiagnostic, i);

      for (j = 0; j < diag->locations->len; j++)
        {
          GcaSourceRange *range;

          range = &g_array_index (diag->locations, GcaSourceRange, j);
          gb_editor_document_add_diagnostic (document, diag, range);
        }
    }

  g_array_unref (ar);
}

static gboolean
gb_editor_document_should_trim_line (GbEditorDocument *document,
                                     guint             line)
{
  GbSourceChangeFlags flags;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);

  flags = gb_source_change_monitor_get_line (document->priv->change_monitor,
                                             line);

  return !!flags;
}

static gboolean
text_iter_is_space (const GtkTextIter *iter)
{
  return g_unichar_isspace (gtk_text_iter_get_char (iter));
}

static void
gb_editor_document_trim (GbEditorDocument *document)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  gint line;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  buffer = GTK_TEXT_BUFFER (document);

  gtk_text_buffer_get_end_iter (buffer, &iter);

  for (line = gtk_text_iter_get_line (&iter); line >= 0; line--)
    {
      if (gb_editor_document_should_trim_line (document, line))
        {
          gtk_text_buffer_get_iter_at_line (buffer, &iter, line);

          if (gtk_text_iter_forward_to_line_end (&iter) &&
              text_iter_is_space (&iter))
            {
              GtkTextIter begin = iter;

              while (text_iter_is_space (&begin))
                {
                  if (gtk_text_iter_starts_line (&begin))
                    break;

                  if (!gtk_text_iter_backward_char (&begin))
                    break;
                }

              if (!text_iter_is_space (&begin) &&
                  !gtk_text_iter_ends_line (&begin))
                gtk_text_iter_forward_char (&begin);

              if (!gtk_text_iter_equal (&begin, &iter))
                gtk_text_buffer_delete (buffer, &begin, &iter);
            }
        }
    }

  EXIT;
}

static void
gb_editor_document_guess_language (GbEditorDocument *document)
{
  GtkSourceLanguageManager *manager;
  GtkSourceLanguage *lang;
  GtkTextIter begin;
  GtkTextIter end;
  gboolean result_uncertain = TRUE;
  GFile *location;
  gchar *name = NULL;
  gchar *text = NULL;
  gchar *content_type = NULL;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  location = gtk_source_file_get_location (document->priv->file);
  if (location)
    name = g_file_get_basename (location);

  gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (document), &begin, &end);
  text = gtk_text_iter_get_slice (&begin, &end);

  content_type = g_content_type_guess (name,
                                       (const guint8 *)text, strlen (text),
                                       &result_uncertain);
  if (result_uncertain)
    g_clear_pointer (&content_type, g_free);

  manager = gtk_source_language_manager_get_default ();
  lang = gtk_source_language_manager_guess_language (manager, name, content_type);

  gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (document), lang);

  g_free (content_type);
  g_free (name);
  g_free (text);
}

static void
gb_editor_document_update_title (GbEditorDocument *document)
{
  GbEditorDocumentPrivate *priv;
  GFile *location;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  priv = document->priv;

  g_clear_pointer (&priv->title, g_free);

  location = gtk_source_file_get_location (priv->file);

  if (location)
    {
      priv->title = g_file_get_basename (location);

      if (document->priv->read_only)
        {
          gchar *tmp = priv->title;
          priv->title = g_strdup_printf (_("%s (Read Only)"), tmp);
          g_free (tmp);
        }
    }
  else
    priv->title = g_strdup_printf (_("untitled document %u"), priv->doc_seq_id);

  g_object_notify (G_OBJECT (document), "title");
}

static void
gb_editor_document_notify_file_location (GbEditorDocument *document,
                                         GParamSpec       *pspec,
                                         GtkSourceFile    *file)
{
  GbEditorDocumentPrivate *priv;
  GFile *location;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (GTK_SOURCE_IS_FILE (file));

  priv = document->priv;

  location = gtk_source_file_get_location (file);

  if (!location)
    {
      if (!priv->doc_seq_id)
        {
          priv->doc_seq_id = gb_doc_seq_acquire ();
          g_get_current_time (&priv->unsaved_ctime);
        }
    }
  else
    {
      if (priv->doc_seq_id)
        {
          gb_doc_seq_release (priv->doc_seq_id);
          priv->doc_seq_id = 0;
        }
    }

  gb_editor_document_update_title (document);

  gb_source_change_monitor_set_file (priv->change_monitor, location);

  gb_editor_document_guess_language (document);
}

static void
gb_editor_document_progress_cb (goffset  current_num_bytes,
                                goffset  total_num_bytes,
                                gpointer user_data)
{
  GbEditorDocument *document = user_data;
  gdouble fraction;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  fraction = total_num_bytes
           ? ((gdouble)current_num_bytes / (gdouble)total_num_bytes)
           : 1.0;

  gb_editor_document_set_progress (document, fraction);
}

static void
gb_editor_document_load_info_cb (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  GbEditorDocument *document = user_data;
  GFileInfo *info;
  GError *error = NULL;
  GFile *file = (GFile *)object;

  g_return_if_fail (G_IS_FILE (file));
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  info = g_file_query_info_finish (file, result, &error);

  if (info)
    {
      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
        {
          gboolean read_only;

          read_only = !g_file_info_get_attribute_boolean (info,
                                                          G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
          gb_editor_document_set_read_only (document, read_only);
        }

      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        {
          GTimeVal tv;

          g_file_info_get_modification_time (info, &tv);

          document->priv->mtime = tv;
          document->priv->mtime_set = TRUE;
        }
    }

  g_clear_object (&document);
  g_clear_object (&info);
}

static GFile *
gb_editor_document_prompt_save (GbEditorDocument *document,
                                GtkWidget        *toplevel)
{
  const gchar *title;
  GtkDialog *dialog;
  GtkWidget *suggested;
  gint response;
  GFile *chosen_file = NULL;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);
  g_return_val_if_fail (!toplevel || GTK_IS_WIDGET (toplevel), NULL);

  dialog = g_object_new (GTK_TYPE_FILE_CHOOSER_DIALOG,
                         "action", GTK_FILE_CHOOSER_ACTION_SAVE,
                         "do-overwrite-confirmation", TRUE,
                         "local-only", FALSE,
                         "select-multiple", FALSE,
                         "show-hidden", FALSE,
                         "transient-for", toplevel,
                         "title", _("Save Document As"),
                         NULL);

  title = gb_document_get_title (GB_DOCUMENT (document));
  gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), title);

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
                          _("Cancel"), GTK_RESPONSE_CANCEL,
                          _("Save"), GTK_RESPONSE_OK,
                          NULL);

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

  suggested = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog),
                                                  GTK_RESPONSE_OK);
  gtk_style_context_add_class (gtk_widget_get_style_context (suggested),
                               GTK_STYLE_CLASS_SUGGESTED_ACTION);

  response = gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_hide (GTK_WIDGET (dialog));

  if (response == GTK_RESPONSE_OK)
    {
      chosen_file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
      gtk_source_file_set_location (document->priv->file, chosen_file);
    }

  gtk_widget_destroy (GTK_WIDGET (dialog));

  return chosen_file;
}

static void
gb_editor_document_save_cb (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  GtkSourceFileSaver *saver = (GtkSourceFileSaver *)object;
  GbSourceChangeMonitor *change_monitor;
  GbEditorDocument *document;
  GError *error = NULL;
  GTask *task = user_data;
  GFile *location;

  ENTRY;

  g_return_if_fail (GTK_SOURCE_IS_FILE_SAVER (saver));
  g_return_if_fail (G_IS_ASYNC_RESULT (result));
  g_return_if_fail (G_IS_TASK (task));

  document = g_task_get_source_object (task);

  if (!gtk_source_file_saver_save_finish (saver, result, &error))
    {
      gb_editor_document_set_error (document, error);
      g_task_return_error (task, error);
      GOTO (cleanup);
    }

  /*
   * FIXME:
   *
   *   Technically this can race. We need to either disable the editing
   *   for the buffer during the process or keep a sequence number to
   *   ensure it hasn't changed since we started the request to save.
   */
  gtk_text_buffer_set_modified (GTK_TEXT_BUFFER (document), FALSE);

  location = gtk_source_file_saver_get_location (saver);
  g_file_query_info_async (location,
                           G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE","
                           G_FILE_ATTRIBUTE_TIME_MODIFIED,
                           G_FILE_QUERY_INFO_NONE,
                           G_PRIORITY_DEFAULT,
                           document->priv->cancellable,
                           gb_editor_document_load_info_cb,
                           g_object_ref (document));

  change_monitor = gb_editor_document_get_change_monitor (document);
  gb_source_change_monitor_reload (change_monitor);

  g_task_return_boolean (task, TRUE);

  g_signal_emit (document, gSignals [SAVED], 0);

cleanup:
  g_object_unref (task);

  EXIT;
}

static void
gb_editor_document_save_async (GbDocument          *doc,
                               GtkWidget           *toplevel,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  GtkSourceFileSaver *saver;
  GbEditorDocument *document = (GbEditorDocument *)doc;
  GbEditorFileMarks *marks;
  GbEditorFileMark *mark;
  GFile *location;
  GTask *task;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  if (!(location = gtk_source_file_get_location (document->priv->file)))
    {
      GFile *chosen_file;

      chosen_file = gb_editor_document_prompt_save (document, toplevel);

      if (!chosen_file)
        {
          g_task_report_new_error (document, callback, user_data,
                                   gb_editor_document_save_async,
                                   G_IO_ERROR,
                                   G_IO_ERROR_NOT_FOUND,
                                   _("No file was selected."));
          EXIT;
        }

      location = gtk_source_file_get_location (document->priv->file);
      g_assert (location == chosen_file);

      g_clear_object (&chosen_file);
    }

  task = g_task_new (document, cancellable, callback, user_data);

  if (document->priv->trim_trailing_whitespace)
    gb_editor_document_trim (document);

  saver = gtk_source_file_saver_new (GTK_SOURCE_BUFFER (document),
                                     document->priv->file);

  location = gtk_source_file_get_location (document->priv->file);

  if (location)
    {
      GtkTextMark *insert;
      GtkTextIter iter;
      guint line;
      guint column;

      marks = gb_editor_file_marks_get_default ();
      mark = gb_editor_file_marks_get_for_file (marks, location);

      insert = gtk_text_buffer_get_insert (GTK_TEXT_BUFFER (document));
      gtk_text_buffer_get_iter_at_mark (GTK_TEXT_BUFFER (document), &iter,
                                        insert);
      line = gtk_text_iter_get_line (&iter);
      column = gtk_text_iter_get_line_offset (&iter);

      gb_editor_file_mark_set_line (mark, line);
      gb_editor_file_mark_set_column (mark, column);
    }

  gb_editor_document_set_progress (document, 0.0);

  document->priv->mtime_set = FALSE;

  gtk_source_file_saver_save_async (saver,
                                    G_PRIORITY_DEFAULT,
                                    cancellable,
                                    gb_editor_document_progress_cb,
                                    g_object_ref (document),
                                    g_object_unref,
                                    gb_editor_document_save_cb,
                                    task);

  g_object_unref (saver);

  EXIT;
}

static gboolean
gb_editor_document_save_finish (GbDocument    *document,
                                GAsyncResult  *result,
                                GError       **error)
{
  GTask *task = (GTask *)result;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);
  g_return_val_if_fail (G_IS_TASK (task), FALSE);

  return g_task_propagate_boolean (task, error);
}

static void
gb_editor_document_save_as_cb (GObject      *object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  GbDocument *document;
  GTask *task = user_data;
  GError *error = NULL;

  g_return_if_fail (G_IS_ASYNC_RESULT (result));
  g_return_if_fail (G_IS_TASK (task));

  document = g_task_get_source_object (task);

  if (!gb_editor_document_save_finish (document, result, &error))
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);

  g_object_unref (task);
}

static void
gb_editor_document_save_as_async (GbDocument          *document,
                                  GtkWidget           *toplevel,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  GbEditorDocument *self = (GbEditorDocument *)document;
  GTask *task;
  GFile *chosen_file;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (self));

  task = g_task_new (self, cancellable, callback, user_data);

  chosen_file = gb_editor_document_prompt_save (self, toplevel);

  if (chosen_file)
    {
      gb_editor_document_save_async (GB_DOCUMENT (self),
                                     toplevel,
                                     cancellable,
                                     gb_editor_document_save_as_cb,
                                     task);
      g_clear_object (&chosen_file);
    }
  else
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_NOT_FOUND,
                               _("No file was selected for saving."));
    }

  EXIT;
}

static gboolean
gb_editor_document_save_as_finish (GbDocument    *document,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  GTask *task = (GTask *)result;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);
  g_return_val_if_fail (G_IS_TASK (task), FALSE);

  return g_task_propagate_boolean (task, error);
}

static void
gb_editor_document_restore_insert (GbEditorDocument *document)
{
  GbEditorFileMarks *marks;
  GbEditorFileMark *mark;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GSettings *settings;
  gboolean load_mark;
  GFile *file;
  guint line;
  guint column;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  settings = g_settings_new ("org.gnome.builder.editor");
  load_mark = g_settings_get_boolean (settings, "restore-insert-mark");
  g_clear_object (&settings);

  buffer = GTK_TEXT_BUFFER (document);

  if (!load_mark)
    {
      gtk_text_buffer_get_start_iter (buffer, &iter);
      gtk_text_buffer_select_range (buffer, &iter, &iter);
      return;
    }

  file = gtk_source_file_get_location (document->priv->file);
  if (!file)
    return;

  marks = gb_editor_file_marks_get_default ();
  mark = gb_editor_file_marks_get_for_file (marks, file);

  line = gb_editor_file_mark_get_line (mark);
  column = gb_editor_file_mark_get_column (mark);

  gb_gtk_text_buffer_get_iter_at_line_and_offset (buffer, &iter, line, column);
  gtk_text_buffer_select_range (buffer, &iter, &iter);

  g_signal_emit (document, gSignals [FILE_MARK_SET], 0, &iter);
}

static void
gb_editor_document_load_cb (GObject      *object,
                            GAsyncResult *result,
                            gpointer      user_data)
{
  GtkSourceFileLoader *loader = (GtkSourceFileLoader *)object;
  GbEditorDocument *document;
  GFile *location;
  GError *error = NULL;
  GTask *task = user_data;

  ENTRY;

  g_return_if_fail (GTK_SOURCE_IS_FILE_LOADER (loader));
  g_return_if_fail (G_IS_ASYNC_RESULT (result));
  g_return_if_fail (G_IS_TASK (task));

  document = g_task_get_source_object (task);

  if (!gtk_source_file_loader_load_finish (loader, result, &error))
    {
      gb_editor_document_set_error (document, error);
      g_task_return_error (task, error);
      GOTO (cleanup);
    }

  document->priv->mtime_set = FALSE;
  document->priv->file_changed_on_volume = FALSE;

  location = gtk_source_file_loader_get_location (loader);
  g_file_query_info_async (location,
                           G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE","
                           G_FILE_ATTRIBUTE_TIME_MODIFIED,
                           G_FILE_QUERY_INFO_NONE,
                           G_PRIORITY_DEFAULT,
                           document->priv->cancellable,
                           gb_editor_document_load_info_cb,
                           g_object_ref (document));

  gb_editor_document_restore_insert (document);
  gb_editor_document_guess_language (document);

  g_task_return_boolean (task, TRUE);

cleanup:
  g_object_unref (task);

  EXIT;
}

void
gb_editor_document_load_async (GbEditorDocument      *document,
                               GFile                 *file,
                               GCancellable          *cancellable,
                               GAsyncReadyCallback    callback,
                               gpointer               user_data)
{
  GtkSourceFileLoader *loader;
  GTask *task;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  if (file)
    gtk_source_file_set_location (document->priv->file, file);

  task = g_task_new (document, cancellable, callback, user_data);

  loader = gtk_source_file_loader_new (GTK_SOURCE_BUFFER (document),
                                       document->priv->file);

  gb_editor_document_set_file_changed_on_volume (document, FALSE);
  gb_editor_document_set_progress (document, 0.0);

  gtk_source_file_loader_load_async (loader,
                                     G_PRIORITY_DEFAULT,
                                     cancellable,
                                     gb_editor_document_progress_cb,
                                     g_object_ref (document),
                                     g_object_unref,
                                     gb_editor_document_load_cb,
                                     task);

  g_object_unref (loader);

  EXIT;
}

gboolean
gb_editor_document_load_finish (GbEditorDocument  *document,
                                GAsyncResult      *result,
                                GError           **error)
{
  GTask *task = (GTask *)result;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);
  g_return_val_if_fail (G_IS_TASK (task), FALSE);

  return g_task_propagate_boolean (task, error);
}

void
gb_editor_document_reload (GbEditorDocument *document)
{
  GFile *location;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  location = gtk_source_file_get_location (document->priv->file);

  if (!location)
    {
      g_warning ("Cannot reload document as it has not been saved to disk.");
      return;
    }

  gb_editor_document_load_async (document, location, NULL, NULL, NULL);
}

static void
gb_editor_document_modified_changed (GtkTextBuffer *buffer)
{
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (buffer));

  if (GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->modified_changed)
    GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->
      modified_changed (buffer);

  g_object_notify (G_OBJECT (buffer), "modified");
}

gboolean
gb_editor_document_get_mtime (GbDocument *document,
                              GTimeVal   *mtime)
{
  GbEditorDocument *self = (GbEditorDocument *)document;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (self), FALSE);

  if (self->priv->doc_seq_id)
    {
      memcpy (mtime, &self->priv->unsaved_ctime, sizeof *mtime);
      return TRUE;
    }

  if (self->priv->mtime_set)
    {
      memcpy (mtime, &self->priv->mtime, sizeof *mtime);
      return TRUE;
    }

  return FALSE;
}

gboolean
gb_editor_document_get_modified (GbDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), FALSE);

  return gtk_text_buffer_get_modified (GTK_TEXT_BUFFER (document));
}

const gchar *
gb_editor_document_get_title (GbDocument *document)
{
  GbEditorDocument *self = (GbEditorDocument *)document;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (self), NULL);

  return self->priv->title;
}

static GtkWidget *
gb_editor_document_create_view (GbDocument *document)
{
  GbEditorView *view;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  view = g_object_new (GB_TYPE_EDITOR_VIEW,
                       "document", document,
                       "visible", TRUE,
                       NULL);

  return GTK_WIDGET (view);
}

static void
gb_editor_document_constructed (GObject *object)
{
  GbEditorDocument *self = (GbEditorDocument *)object;

  G_OBJECT_CLASS (gb_editor_document_parent_class)->constructed (object);

  gb_editor_document_notify_file_location (self, NULL, self->priv->file);
}

static void
gb_editor_document_dispose (GObject *object)
{
  GbEditorDocument *document = (GbEditorDocument *)object;

  if (!g_cancellable_is_cancelled (document->priv->cancellable))
    g_cancellable_cancel (document->priv->cancellable);

  G_OBJECT_CLASS (gb_editor_document_parent_class)->dispose (object);
}

static void
gb_editor_document_finalize (GObject *object)
{
  GbEditorDocumentPrivate *priv = GB_EDITOR_DOCUMENT (object)->priv;

  ENTRY;

  if (priv->doc_seq_id)
    {
      gb_doc_seq_release (priv->doc_seq_id);
      priv->doc_seq_id = 0;
    }

  g_clear_object (&priv->file);
  g_clear_object (&priv->change_monitor);
  g_clear_object (&priv->code_assistant);
  g_clear_object (&priv->cancellable);
  g_clear_pointer (&priv->title, g_free);

  G_OBJECT_CLASS(gb_editor_document_parent_class)->finalize (object);

  EXIT;
}

static void
gb_editor_document_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GbEditorDocument *self = (GbEditorDocument *)object;

  switch (prop_id)
    {
    case PROP_MODIFIED:
      g_value_set_boolean (value,
                           gb_editor_document_get_modified (GB_DOCUMENT (self)));
      break;

    case PROP_CHANGE_MONITOR:
      g_value_set_object (value, gb_editor_document_get_change_monitor (self));
      break;

    case PROP_ERROR:
      g_value_set_boxed (value, gb_editor_document_get_error (self));
      break;

    case PROP_FILE:
      g_value_set_object (value, gb_editor_document_get_file (self));
      break;

    case PROP_FILE_CHANGED_ON_VOLUME:
      g_value_set_boolean (value,
                           gb_editor_document_get_file_changed_on_volume (self));
      break;

    case PROP_READ_ONLY:
      g_value_set_boolean (value,
                           gb_editor_document_get_read_only (GB_DOCUMENT (self)));
      break;

    case PROP_PROGRESS:
      g_value_set_double (value, gb_editor_document_get_progress (self));
      break;

    case PROP_TITLE:
      g_value_set_string (value,
                          gb_editor_document_get_title (GB_DOCUMENT (self)));
      break;

    case PROP_TRIM_TRAILING_WHITESPACE:
      g_value_set_boolean (value,
                           gb_editor_document_get_trim_trailing_whitespace (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gb_editor_document_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GbEditorDocument *self = (GbEditorDocument *)object;

  switch (prop_id)
    {
    case PROP_STYLE_SCHEME_NAME:
      gb_editor_document_set_style_scheme_name (self,
                                                g_value_get_string (value));
      break;

    case PROP_TRIM_TRAILING_WHITESPACE:
      gb_editor_document_set_trim_trailing_whitespace (self,
                                                       g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gb_editor_document_class_init (GbEditorDocumentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkTextBufferClass *text_buffer_class = GTK_TEXT_BUFFER_CLASS (klass);

  object_class->constructed = gb_editor_document_constructed;
  object_class->dispose = gb_editor_document_dispose;
  object_class->finalize = gb_editor_document_finalize;
  object_class->get_property = gb_editor_document_get_property;
  object_class->set_property = gb_editor_document_set_property;

  text_buffer_class->mark_set = gb_editor_document_mark_set;
  text_buffer_class->changed = gb_editor_document_changed;
  text_buffer_class->modified_changed = gb_editor_document_modified_changed;

  g_object_class_override_property (object_class, PROP_MODIFIED, "modified");
  g_object_class_override_property (object_class, PROP_READ_ONLY, "read-only");
  g_object_class_override_property (object_class, PROP_TITLE, "title");

  gParamSpecs [PROP_CHANGE_MONITOR] =
    g_param_spec_object ("change-monitor",
                         _("Change Monitor"),
                         _("The change monitor for the backing file."),
                         GB_TYPE_SOURCE_CHANGE_MONITOR,
                         (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_CHANGE_MONITOR,
                                   gParamSpecs [PROP_CHANGE_MONITOR]);

  gParamSpecs [PROP_ERROR] =
    g_param_spec_boxed ("error",
                        _("Error"),
                        _("An error that may have been loaded."),
                        G_TYPE_ERROR,
                        (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ERROR,
                                   gParamSpecs [PROP_ERROR]);

  gParamSpecs [PROP_FILE] =
    g_param_spec_object ("file",
                         _("File"),
                         _("The backing file for the document."),
                         GTK_SOURCE_TYPE_FILE,
                         (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FILE,
                                   gParamSpecs [PROP_FILE]);

  gParamSpecs [PROP_FILE_CHANGED_ON_VOLUME] =
    g_param_spec_boolean ("file-changed-on-volume",
                          _("File Changed on Volume"),
                          _("If the file has changed underneath the buffer."),
                          FALSE,
                          (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FILE_CHANGED_ON_VOLUME,
                                   gParamSpecs [PROP_FILE_CHANGED_ON_VOLUME]);

  gParamSpecs [PROP_PROGRESS] =
    g_param_spec_double ("progress",
                         _("Progress"),
                         _("Loading or saving progress."),
                         0.0,
                         1.0,
                         0.0,
                         (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_PROGRESS,
                                   gParamSpecs [PROP_PROGRESS]);

  gParamSpecs [PROP_STYLE_SCHEME_NAME] =
    g_param_spec_string ("style-scheme-name",
                         _("Style Scheme Name"),
                         _("The style scheme name."),
                         NULL,
                         (G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_STYLE_SCHEME_NAME,
                                   gParamSpecs [PROP_STYLE_SCHEME_NAME]);

  gParamSpecs [PROP_TRIM_TRAILING_WHITESPACE] =
    g_param_spec_boolean ("trim-trailing-whitespace",
                         _("Trim Trailing Whitespace"),
                         _("If whitespace should be trimmed before saving."),
                         TRUE,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_TRIM_TRAILING_WHITESPACE,
                                   gParamSpecs [PROP_TRIM_TRAILING_WHITESPACE]);

  gSignals [CURSOR_MOVED] =
    g_signal_new ("cursor-moved",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbEditorDocumentClass, cursor_moved),
                  NULL,
                  NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  gSignals [FILE_MARK_SET] =
    g_signal_new ("file-mark-set",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbEditorDocumentClass, file_mark_set),
                  NULL, NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_NONE,
                  1,
                  GTK_TYPE_TEXT_ITER);

  gSignals [SAVED] =
    g_signal_new ("saved",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbEditorDocumentClass, saved),
                  NULL, NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_NONE,
                  0);
}

static void
gb_editor_document_init (GbEditorDocument *document)
{
  document->priv = gb_editor_document_get_instance_private (document);

  document->priv->cancellable = g_cancellable_new ();
  document->priv->trim_trailing_whitespace = TRUE;
  document->priv->file = gtk_source_file_new ();
  document->priv->change_monitor = gb_source_change_monitor_new (GTK_TEXT_BUFFER (document));
  document->priv->code_assistant = gb_source_code_assistant_new (GTK_TEXT_BUFFER (document));

  g_signal_connect_object (document->priv->file,
                           "notify::location",
                           G_CALLBACK (gb_editor_document_notify_file_location),
                           document,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (document->priv->code_assistant,
                           "changed",
                           G_CALLBACK (gb_editor_document_code_assistant_changed),
                           document,
                           G_CONNECT_SWAPPED);

  g_signal_connect (document,
                    "notify::style-scheme",
                    G_CALLBACK (gb_editor_document_notify_style_scheme),
                    NULL);
}

static void
gb_editor_document_init_document (GbDocumentInterface *iface)
{
  iface->get_modified = gb_editor_document_get_modified;
  iface->get_mtime = gb_editor_document_get_mtime;
  iface->get_read_only = gb_editor_document_get_read_only;
  iface->get_title = gb_editor_document_get_title;
  iface->is_untitled = gb_editor_document_is_untitled;
  iface->create_view = gb_editor_document_create_view;
  iface->save_async = gb_editor_document_save_async;
  iface->save_finish = gb_editor_document_save_finish;
  iface->save_as_async = gb_editor_document_save_as_async;
  iface->save_as_finish = gb_editor_document_save_as_finish;
}
