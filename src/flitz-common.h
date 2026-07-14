/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FLITZ_COMMON_H
#define FLITZ_COMMON_H

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <libintl.h>

#define FLITZ_VERSION "6.0"
#define FLITZ_PREFIX "/usr/local/flitz"
#define FLITZ_ICON FLITZ_PREFIX "/Flitz.png"
#define FLITZ_LOCALE_DIR "/usr/share/locale"
#define FLITZ_GETTEXT_PACKAGE "flitz-toolkit"

typedef struct {
    GtkWidget *window;
    GtkWidget *console_view;
    GtkTextBuffer *console_buffer;
    GtkWidget *progress_bar;
    GtkWidget *progress_label;
} FlitzUi;

typedef void (*FlitzLineFunc)(const gchar *line, gpointer user_data);

void flitz_init_i18n(const gchar *language);
void flitz_set_window_icon(GtkWindow *window);
GtkWidget *flitz_create_logo(gint size);
void flitz_show_about(GtkWindow *parent, const gchar *program_name);
void flitz_ui_init(FlitzUi *ui, GtkWidget *window, GtkWidget *console_view,
                   GtkWidget *progress_bar, GtkWidget *progress_label);
void flitz_log(FlitzUi *ui, const gchar *text);
void flitz_logf(FlitzUi *ui, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
void flitz_queue_log(FlitzUi *ui, const gchar *text);
void flitz_set_progress(FlitzUi *ui, gdouble fraction, const gchar *text);
void flitz_queue_progress(FlitzUi *ui, gdouble fraction, const gchar *text,
                          gboolean pulse);
void flitz_clear_console(FlitzUi *ui);

gboolean flitz_command_exists(const gchar *command);
gboolean flitz_run_command(const gchar *const argv[], const gchar *cwd,
                           FlitzLineFunc line_func, gpointer user_data,
                           GError **error);
gboolean flitz_run_command_quiet(const gchar *const argv[], const gchar *cwd);

gchar *flitz_first_dropped_path(GtkSelectionData *selection_data);
gchar *flitz_path_dirname(const gchar *path);
gchar *flitz_path_basename(const gchar *path);
gchar *flitz_archive_basename(const gchar *path);
gchar *flitz_archive_extension(const gchar *path);
gboolean flitz_is_supported_archive(const gchar *path);

gboolean flitz_ensure_directory(const gchar *path, GError **error);
gboolean flitz_remove_tree(const gchar *path, GError **error);
gboolean flitz_commit_stage(const gchar *stage_dir, const gchar *output_dir,
                            gboolean keep_structure, gboolean overwrite,
                            FlitzLineFunc line_func, gpointer user_data,
                            GError **error);
guint64 flitz_count_entries(const gchar *path);

gchar *flitz_short_path(const gchar *path, gsize max_chars);
gint flitz_parse_percent(const gchar *line);

#endif
