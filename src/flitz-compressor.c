/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "flitz-common.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    FORMAT_TAR_GZ,
    FORMAT_TAR_XZ,
    FORMAT_ZIP,
    FORMAT_7Z,
    FORMAT_SQUASHFS
} CompressionFormat;

typedef struct {
    FlitzUi ui;
    GtkWidget *source_label;
    GtkWidget *output_label;
    GtkWidget *output_entry;
    GtkWidget *format_combo;
    GtkWidget *squash_frame;
    GtkWidget *compression_combo;
    GtkWidget *block_combo;
    GtkWidget *xz_frame;
    GtkWidget *xz_combo;
    GtkWidget *compress_button;
    gchar *source_path;
    gchar *output_dir;
    gboolean running;
} CompressorApp;

typedef struct {
    CompressorApp *app;
    gchar *source_path;
    gchar *output_path;
    gchar *output_dir;
    gchar *output_name;
    CompressionFormat format;
    gchar *squash_compression;
    gchar *block_size;
    gchar *xz_filter;
    guint64 total_entries;
    guint64 processed_lines;
} CompressJob;

typedef struct {
    CompressorApp *app;
    gboolean success;
    gchar *message;
} CompressFinish;

static const gchar *format_names[] = {
    "tar.gz", "tar.xz", "zip", "7z", "squashfs"
};

static const gchar *squash_compressions[] = {
    "gzip", "xz", "lz4", "zstd"
};

static const gchar *block_sizes[] = {
    "4k", "16k", "32k", "64k", "128k", "256k", "512k", "1m", "1024k"
};

static const gchar *xz_filters[] = {
    "None", "x86", "powerpc", "ia64", "arm", "armthumb"
};

static void compressor_set_source(CompressorApp *app, const gchar *path);
static void compressor_update_fields(CompressorApp *app);

static void show_message(GtkWindow *parent, GtkMessageType type,
                         const gchar *primary, const gchar *secondary)
{
    GtkWidget *dialog = gtk_message_dialog_new(parent,
                                                GTK_DIALOG_MODAL |
                                                GTK_DIALOG_DESTROY_WITH_PARENT,
                                                type, GTK_BUTTONS_OK,
                                                "%s", primary);
    if (secondary != NULL && *secondary != '\0') {
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                                 "%s", secondary);
    }
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static gboolean confirm_overwrite(GtkWindow *parent, const gchar *path)
{
    GtkWidget *dialog;
    gint response;
    gchar *secondary = g_strdup_printf(
        _("The destination already exists:\n%s\n\nReplace it?"), path);
    dialog = gtk_message_dialog_new(parent,
                                    GTK_DIALOG_MODAL |
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION,
                                    GTK_BUTTONS_YES_NO,
                                    "%s", _("Replace existing file?"));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                             "%s", secondary);
    response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(secondary);
    return response == GTK_RESPONSE_YES;
}

static void compressor_line(const gchar *line, gpointer user_data)
{
    CompressJob *job = user_data;
    gint percent = flitz_parse_percent(line);
    flitz_queue_log(&job->app->ui, line);
    if (percent >= 0) {
        gchar *label = g_strdup_printf(_("%d%% completed"), percent);
        flitz_queue_progress(&job->app->ui, percent / 100.0, label, FALSE);
        g_free(label);
        return;
    }

    if (job->format == FORMAT_TAR_GZ || job->format == FORMAT_TAR_XZ ||
        job->format == FORMAT_ZIP) {
        job->processed_lines++;
        if (job->total_entries > 0) {
            gdouble fraction = (gdouble)job->processed_lines /
                               (gdouble)job->total_entries;
            gint display = (gint)(CLAMP(fraction, 0.0, 0.99) * 100.0);
            gchar *label = g_strdup_printf(_("%d%% completed"), display);
            flitz_queue_progress(&job->app->ui, fraction, label, FALSE);
            g_free(label);
            return;
        }
    }
    flitz_queue_progress(&job->app->ui, 0.0, _("Compressing..."), TRUE);
}

static gboolean run_compression_command(CompressJob *job,
                                        const gchar *const argv[],
                                        const gchar *cwd,
                                        GError **error)
{
    GString *command = g_string_new(NULL);
    guint i;
    gboolean ok;
    for (i = 0; argv[i] != NULL; i++) {
        gchar *quoted = g_shell_quote(argv[i]);
        if (i > 0) {
            g_string_append_c(command, ' ');
        }
        g_string_append(command, quoted);
        g_free(quoted);
    }
    {
        gchar *message = g_strdup_printf(_("Running: %s"), command->str);
        flitz_queue_log(&job->app->ui, message);
        g_free(message);
    }
    g_string_free(command, TRUE);
    ok = flitz_run_command(argv, cwd, compressor_line, job, error);
    return ok;
}

static gboolean create_archive(CompressJob *job, GError **error)
{
    gchar *parent = g_path_get_dirname(job->source_path);
    gchar *base = g_path_get_basename(job->source_path);
    gboolean ok = FALSE;

    switch (job->format) {
    case FORMAT_TAR_GZ: {
        const gchar *argv[] = { "tar", "-czvf", job->output_path,
                                "-C", parent, base, NULL };
        if (!flitz_command_exists("tar")) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                        "%s", _("Required tool is not installed: tar"));
        } else {
            ok = run_compression_command(job, argv, NULL, error);
        }
        break;
    }
    case FORMAT_TAR_XZ: {
        const gchar *argv[] = { "tar", "-cJvf", job->output_path,
                                "-C", parent, base, NULL };
        if (!flitz_command_exists("tar")) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                        "%s", _("Required tool is not installed: tar"));
        } else {
            ok = run_compression_command(job, argv, NULL, error);
        }
        break;
    }
    case FORMAT_ZIP: {
        const gchar *argv[] = { "zip", "-r", job->output_path, base, NULL };
        if (!flitz_command_exists("zip")) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                        "%s", _("Required tool is not installed: zip"));
        } else {
            ok = run_compression_command(job, argv, parent, error);
        }
        break;
    }
    case FORMAT_7Z: {
        const gchar *argv[] = { "7z", "a", "-y", "-bsp1",
                                job->output_path, base, NULL };
        if (!flitz_command_exists("7z")) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                        "%s", _("Required tool is not installed: 7z"));
        } else {
            ok = run_compression_command(job, argv, parent, error);
        }
        break;
    }
    case FORMAT_SQUASHFS: {
        const gchar *argv_with_filter[] = {
            "mksquashfs", job->source_path, job->output_path,
            "-noappend", "-comp", job->squash_compression,
            "-b", job->block_size, "-progress", "-Xbcj",
            job->xz_filter, NULL
        };
        const gchar *argv_plain[] = {
            "mksquashfs", job->source_path, job->output_path,
            "-noappend", "-comp", job->squash_compression,
            "-b", job->block_size, "-progress", NULL
        };
        if (!flitz_command_exists("mksquashfs")) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                        "%s", _("mksquashfs is not installed."));
        } else if (g_strcmp0(job->squash_compression, "xz") == 0 &&
                   g_strcmp0(job->xz_filter, "None") != 0) {
            ok = run_compression_command(job, argv_with_filter, NULL, error);
        } else {
            ok = run_compression_command(job, argv_plain, NULL, error);
        }
        break;
    }
    default:
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "%s", _("Format not supported"));
        break;
    }

    g_free(parent);
    g_free(base);
    return ok;
}

static gboolean compression_finished_idle(gpointer data)
{
    CompressFinish *finish = data;
    CompressorApp *app = finish->app;
    app->running = FALSE;
    gtk_widget_set_sensitive(app->compress_button, TRUE);
    if (finish->success) {
        flitz_set_progress(&app->ui, 1.0, _("Completed successfully"));
        flitz_log(&app->ui, _("Completed successfully."));
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_INFO,
                     _("Success"), finish->message);
    } else {
        flitz_set_progress(&app->ui, 0.0, _("Compression failed"));
        flitz_logf(&app->ui, _("Error: %s"), finish->message);
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                     _("Compression failed"), finish->message);
    }
    g_free(finish->message);
    g_free(finish);
    return G_SOURCE_REMOVE;
}

static gpointer compress_worker(gpointer data)
{
    CompressJob *job = data;
    CompressFinish *finish = g_new0(CompressFinish, 1);
    GError *error = NULL;
    gboolean ok;

    finish->app = job->app;
    job->total_entries = flitz_count_entries(job->source_path);
    ok = create_archive(job, &error);
    if (!ok && g_file_test(job->output_path, G_FILE_TEST_EXISTS)) {
        GError *cleanup_error = NULL;
        flitz_remove_tree(job->output_path, &cleanup_error);
        g_clear_error(&cleanup_error);
    }
    finish->success = ok;
    if (ok) {
        finish->message = g_strdup_printf(_("File created:\n%s"),
                                          job->output_path);
    } else {
        finish->message = g_strdup(error != NULL ? error->message :
                                   _("Unknown compression error."));
    }
    g_clear_error(&error);

    g_free(job->source_path);
    g_free(job->output_path);
    g_free(job->output_dir);
    g_free(job->output_name);
    g_free(job->squash_compression);
    g_free(job->block_size);
    g_free(job->xz_filter);
    g_free(job);
    g_idle_add(compression_finished_idle, finish);
    return NULL;
}

static gchar *combo_active_text(GtkWidget *combo)
{
    return gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
}

static void compressor_start(CompressorApp *app)
{
    const gchar *output_name;
    gchar *output_path;
    CompressJob *job;
    gint format_index;

    if (app->running) {
        return;
    }
    if (app->source_path == NULL ||
        !g_file_test(app->source_path, G_FILE_TEST_IS_DIR)) {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                     _("Error"), _("Please select a valid folder first."));
        return;
    }
    output_name = gtk_entry_get_text(GTK_ENTRY(app->output_entry));
    if (output_name == NULL || *output_name == '\0') {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                     _("Error"), _("Enter an output filename."));
        return;
    }
    if (strchr(output_name, G_DIR_SEPARATOR) != NULL) {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                     _("Error"),
                     _("The output filename must not contain a slash."));
        return;
    }
    if (app->output_dir == NULL || *app->output_dir == '\0') {
        g_free(app->output_dir);
        app->output_dir = flitz_path_dirname(app->source_path);
    }
    {
        GError *error = NULL;
        if (!flitz_ensure_directory(app->output_dir, &error)) {
            show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                         _("Error"), error->message);
            g_clear_error(&error);
            return;
        }
    }
    output_path = g_build_filename(app->output_dir, output_name, NULL);
    {
        gchar *source_prefix = g_strconcat(app->source_path, G_DIR_SEPARATOR_S, NULL);
        gboolean inside_source = g_str_has_prefix(output_path, source_prefix);
        g_free(source_prefix);
        if (inside_source) {
            show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                         _("Error"),
                         _("Choose an output directory outside the source folder."));
            g_free(output_path);
            return;
        }
    }
    if (g_file_test(output_path, G_FILE_TEST_EXISTS)) {
        GError *error = NULL;
        if (!confirm_overwrite(GTK_WINDOW(app->ui.window), output_path)) {
            g_free(output_path);
            return;
        }
        if (!flitz_remove_tree(output_path, &error)) {
            show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                         _("Error"), error->message);
            g_clear_error(&error);
            g_free(output_path);
            return;
        }
    }

    format_index = gtk_combo_box_get_active(GTK_COMBO_BOX(app->format_combo));
    if (format_index < 0 || format_index > FORMAT_SQUASHFS) {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                     _("Error"), _("Format not supported"));
        g_free(output_path);
        return;
    }

    app->running = TRUE;
    gtk_widget_set_sensitive(app->compress_button, FALSE);
    flitz_clear_console(&app->ui);
    flitz_set_progress(&app->ui, 0.0, _("Starting compression..."));
    flitz_logf(&app->ui, _("Source: %s"), app->source_path);
    flitz_logf(&app->ui, _("Output: %s"), output_path);

    job = g_new0(CompressJob, 1);
    job->app = app;
    job->source_path = g_strdup(app->source_path);
    job->output_path = output_path;
    job->output_dir = g_strdup(app->output_dir);
    job->output_name = g_strdup(output_name);
    job->format = (CompressionFormat)format_index;
    job->squash_compression = combo_active_text(app->compression_combo);
    job->block_size = combo_active_text(app->block_combo);
    job->xz_filter = combo_active_text(app->xz_combo);
    if (job->squash_compression == NULL) {
        job->squash_compression = g_strdup("xz");
    }
    if (job->block_size == NULL) {
        job->block_size = g_strdup("128k");
    }
    if (job->xz_filter == NULL) {
        job->xz_filter = g_strdup("None");
    }
    {
        GThread *thread = g_thread_new("flitz-compress", compress_worker, job);
        g_thread_unref(thread);
    }
}

static void on_compress_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    compressor_start(user_data);
}

static void compressor_set_output_dir(CompressorApp *app, const gchar *path)
{
    gchar *short_path;
    g_free(app->output_dir);
    app->output_dir = g_canonicalize_filename(path, NULL);
    short_path = flitz_short_path(app->output_dir, 42);
    gtk_label_set_text(GTK_LABEL(app->output_label), short_path);
    g_free(short_path);
}

static void compressor_suggest_name(CompressorApp *app)
{
    gchar *base;
    gint format_index;
    gchar *name;
    if (app->source_path == NULL) {
        return;
    }
    base = g_path_get_basename(app->source_path);
    format_index = gtk_combo_box_get_active(GTK_COMBO_BOX(app->format_combo));
    if (format_index < 0 || format_index > FORMAT_SQUASHFS) {
        format_index = FORMAT_TAR_GZ;
    }
    if (format_index == FORMAT_SQUASHFS) {
        name = g_strconcat(base, ".sfs", NULL);
    } else {
        name = g_strconcat(base, ".", format_names[format_index], NULL);
    }
    gtk_entry_set_text(GTK_ENTRY(app->output_entry), name);
    g_free(name);
    g_free(base);
}

static void compressor_set_source(CompressorApp *app, const gchar *path)
{
    gchar *canonical;
    gchar *base;
    gchar *short_name;
    gchar *parent;
    if (path == NULL) {
        return;
    }
    canonical = g_canonicalize_filename(path, NULL);
    if (!g_file_test(canonical, G_FILE_TEST_IS_DIR)) {
        g_free(canonical);
        return;
    }
    g_free(app->source_path);
    app->source_path = canonical;
    base = g_path_get_basename(app->source_path);
    short_name = flitz_short_path(base, 35);
    gtk_label_set_text(GTK_LABEL(app->source_label), short_name);
    parent = flitz_path_dirname(app->source_path);
    compressor_set_output_dir(app, parent);
    compressor_suggest_name(app);
    g_free(parent);
    g_free(short_name);
    g_free(base);
}

static void compressor_update_fields(CompressorApp *app)
{
    gint format_index = gtk_combo_box_get_active(
        GTK_COMBO_BOX(app->format_combo));
    gchar *compression = combo_active_text(app->compression_combo);
    gboolean squash = format_index == FORMAT_SQUASHFS;
    gboolean xz = squash && g_strcmp0(compression, "xz") == 0;
    gtk_widget_set_visible(app->squash_frame, squash);
    gtk_widget_set_visible(app->xz_frame, xz);
    compressor_suggest_name(app);
    g_free(compression);
}

static void on_format_changed(GtkComboBox *combo, gpointer user_data)
{
    (void)combo;
    compressor_update_fields(user_data);
}

static void on_select_source(GtkButton *button, gpointer user_data)
{
    CompressorApp *app = user_data;
    GtkWidget *dialog;
    (void)button;
    dialog = gtk_file_chooser_dialog_new(_("Select folder to compress"),
                                         GTK_WINDOW(app->ui.window),
                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                         _("Cancel"), GTK_RESPONSE_CANCEL,
                                         _("Select"), GTK_RESPONSE_ACCEPT,
                                         NULL);
    if (app->source_path != NULL) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog),
                                      app->source_path);
    }
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        compressor_set_source(app, path);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void on_select_output(GtkButton *button, gpointer user_data)
{
    CompressorApp *app = user_data;
    GtkWidget *dialog;
    (void)button;
    dialog = gtk_file_chooser_dialog_new(_("Select output directory"),
                                         GTK_WINDOW(app->ui.window),
                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                         _("Cancel"), GTK_RESPONSE_CANCEL,
                                         _("Select"), GTK_RESPONSE_ACCEPT,
                                         NULL);
    if (app->output_dir != NULL) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog),
                                      app->output_dir);
    }
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        compressor_set_output_dir(app, path);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}

static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context,
                                  gint x, gint y,
                                  GtkSelectionData *selection_data,
                                  guint info, guint time,
                                  gpointer user_data)
{
    CompressorApp *app = user_data;
    gchar *path = flitz_first_dropped_path(selection_data);
    gboolean accepted = FALSE;
    (void)widget;
    (void)x;
    (void)y;
    (void)info;
    if (path != NULL && g_file_test(path, G_FILE_TEST_IS_DIR)) {
        compressor_set_source(app, path);
        accepted = TRUE;
    }
    if (!accepted) {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_WARNING,
                     _("Warning"), _("Please drag only folders."));
    }
    gtk_drag_finish(context, accepted, FALSE, time);
    g_free(path);
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event,
                                 gpointer user_data)
{
    CompressorApp *app = user_data;
    (void)widget;
    (void)event;
    if (app->running) {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_WARNING,
                     _("Operation in progress"),
                     _("Wait for compression to finish before closing Flitz."));
        return TRUE;
    }
    return FALSE;
}

static void on_about_clicked(GtkButton *button, gpointer user_data)
{
    CompressorApp *app = user_data;
    (void)button;
    flitz_show_about(GTK_WINDOW(app->ui.window), _("Flitz Compressor"));
}

static GtkWidget *make_frame(const gchar *title)
{
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 2);
    return frame;
}

static void fill_combo(GtkWidget *combo, const gchar *const values[],
                       guint count, guint active)
{
    guint i;
    for (i = 0; i < count; i++) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), values[i]);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (gint)active);
}

static GtkWidget *build_ui(CompressorApp *app)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *image;
    GtkWidget *title;
    GtkWidget *byline;
    GtkWidget *about_button;
    GtkWidget *source_frame;
    GtkWidget *source_box;
    GtkWidget *source_button;
    GtkWidget *format_frame;
    GtkWidget *format_box;
    GtkWidget *squash_box;
    GtkWidget *xz_box;
    GtkWidget *output_frame;
    GtkWidget *output_box;
    GtkWidget *output_button;
    GtkWidget *name_frame;
    GtkWidget *progress;
    GtkWidget *progress_label;
    GtkWidget *console_frame;
    GtkWidget *scrolled;
    GtkWidget *console;
    GtkWidget *compress_button;
    GtkTargetEntry targets[] = { { "text/uri-list", 0, 0 } };

    gtk_window_set_title(GTK_WINDOW(window), _("Flitz Compressor"));
    gtk_window_set_default_size(GTK_WINDOW(window), 450, 680);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(window), 6);
    flitz_set_window_icon(GTK_WINDOW(window));
    gtk_container_add(GTK_CONTAINER(window), main_box);

    image = flitz_create_logo(48);
    gtk_box_pack_start(GTK_BOX(top_box), image, FALSE, FALSE, 3);
    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), _("<b>Flitz Compressor</b>"));
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    byline = gtk_label_new(_("by josejp2424 (v6.0 C)"));
    gtk_widget_set_halign(byline, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(title_box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_box), byline, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top_box), title_box, TRUE, TRUE, 0);
    about_button = gtk_button_new_with_label(_("About"));
    gtk_widget_set_valign(about_button, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(top_box), about_button, FALSE, FALSE, 3);
    gtk_box_pack_start(GTK_BOX(main_box), top_box, FALSE, FALSE, 0);

    source_frame = make_frame(_("Source"));
    source_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(source_box), 4);
    source_button = gtk_button_new_with_label(_("Select"));
    app->source_label = gtk_label_new(_("Drag folder here or click Select"));
    gtk_widget_set_hexpand(app->source_label, TRUE);
    gtk_widget_set_halign(app->source_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(app->source_label), PANGO_ELLIPSIZE_START);
    gtk_box_pack_start(GTK_BOX(source_box), source_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(source_box), app->source_label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(source_frame), source_box);
    gtk_box_pack_start(GTK_BOX(main_box), source_frame, FALSE, FALSE, 0);

    format_frame = make_frame(_("Format"));
    format_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(format_box), 4);
    app->format_combo = gtk_combo_box_text_new();
    fill_combo(app->format_combo, format_names, G_N_ELEMENTS(format_names), 0);
    gtk_box_pack_start(GTK_BOX(format_box), app->format_combo,
                       FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(format_frame), format_box);
    gtk_box_pack_start(GTK_BOX(main_box), format_frame, FALSE, FALSE, 0);

    app->squash_frame = make_frame(_("SquashFS Options"));
    squash_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(squash_box), 4);
    gtk_box_pack_start(GTK_BOX(squash_box), gtk_label_new(_("Compression:")),
                       FALSE, FALSE, 0);
    app->compression_combo = gtk_combo_box_text_new();
    fill_combo(app->compression_combo, squash_compressions,
               G_N_ELEMENTS(squash_compressions), 1);
    gtk_box_pack_start(GTK_BOX(squash_box), app->compression_combo,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(squash_box), gtk_label_new(_("Block size:")),
                       FALSE, FALSE, 0);
    app->block_combo = gtk_combo_box_text_new();
    fill_combo(app->block_combo, block_sizes, G_N_ELEMENTS(block_sizes), 4);
    gtk_box_pack_start(GTK_BOX(squash_box), app->block_combo,
                       FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->squash_frame), squash_box);
    gtk_box_pack_start(GTK_BOX(main_box), app->squash_frame, FALSE, FALSE, 0);

    app->xz_frame = make_frame(_("Advanced XZ Options"));
    xz_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(xz_box), 4);
    gtk_box_pack_start(GTK_BOX(xz_box), gtk_label_new(_("BCJ Filter:")),
                       FALSE, FALSE, 0);
    app->xz_combo = gtk_combo_box_text_new();
    fill_combo(app->xz_combo, xz_filters, G_N_ELEMENTS(xz_filters), 0);
    gtk_box_pack_start(GTK_BOX(xz_box), app->xz_combo, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(app->xz_frame), xz_box);
    gtk_box_pack_start(GTK_BOX(main_box), app->xz_frame, FALSE, FALSE, 0);

    output_frame = make_frame(_("Output Directory"));
    output_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(output_box), 4);
    output_button = gtk_button_new_with_label(_("Select"));
    app->output_label = gtk_label_new(_("Default: source folder"));
    gtk_widget_set_hexpand(app->output_label, TRUE);
    gtk_widget_set_halign(app->output_label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(app->output_label), PANGO_ELLIPSIZE_START);
    gtk_box_pack_start(GTK_BOX(output_box), output_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(output_box), app->output_label, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(output_frame), output_box);
    gtk_box_pack_start(GTK_BOX(main_box), output_frame, FALSE, FALSE, 0);

    name_frame = make_frame(_("Output Filename"));
    app->output_entry = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(name_frame), app->output_entry);
    gtk_box_pack_start(GTK_BOX(main_box), name_frame, FALSE, FALSE, 0);

    progress = gtk_progress_bar_new();
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(progress), 0.05);
    progress_label = gtk_label_new(_("Ready"));
    gtk_box_pack_start(GTK_BOX(main_box), progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), progress_label, FALSE, FALSE, 0);

    console_frame = make_frame(_("Console"));
    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled, -1, 190);
    console = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(console), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(console), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(console), GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scrolled), console);
    gtk_container_add(GTK_CONTAINER(console_frame), scrolled);
    gtk_box_pack_start(GTK_BOX(main_box), console_frame, TRUE, TRUE, 0);

    compress_button = gtk_button_new_with_label(_("Compress"));
    app->compress_button = compress_button;
    gtk_box_pack_start(GTK_BOX(main_box), compress_button, FALSE, FALSE, 2);

    flitz_ui_init(&app->ui, window, console, progress, progress_label);
    gtk_drag_dest_set(window, GTK_DEST_DEFAULT_ALL, targets, 1,
                      GDK_ACTION_COPY);
    gtk_drag_dest_set(app->source_label, GTK_DEST_DEFAULT_ALL, targets, 1,
                      GDK_ACTION_COPY);
    g_signal_connect(window, "drag-data-received",
                     G_CALLBACK(on_drag_data_received), app);
    g_signal_connect(app->source_label, "drag-data-received",
                     G_CALLBACK(on_drag_data_received), app);
    g_signal_connect(source_button, "clicked", G_CALLBACK(on_select_source),
                     app);
    g_signal_connect(about_button, "clicked", G_CALLBACK(on_about_clicked), app);
    g_signal_connect(output_button, "clicked", G_CALLBACK(on_select_output),
                     app);
    g_signal_connect(app->format_combo, "changed",
                     G_CALLBACK(on_format_changed), app);
    g_signal_connect(app->compression_combo, "changed",
                     G_CALLBACK(on_format_changed), app);
    g_signal_connect(compress_button, "clicked",
                     G_CALLBACK(on_compress_clicked), app);
    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), app);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    return window;
}

static const gchar *scan_language_arg(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if ((g_strcmp0(argv[i], "-l") == 0 ||
             g_strcmp0(argv[i], "--language") == 0) && i + 1 < argc) {
            return argv[i + 1];
        }
        if (g_str_has_prefix(argv[i], "--language=")) {
            return argv[i] + strlen("--language=");
        }
    }
    return NULL;
}

static gchar *scan_initial_path(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "-l") == 0 ||
            g_strcmp0(argv[i], "--language") == 0) {
            i++;
            continue;
        }
        if (g_str_has_prefix(argv[i], "--language=")) {
            continue;
        }
        if (argv[i][0] != '-') {
            return g_strdup(argv[i]);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    CompressorApp *app;
    gchar *initial_path;
    const gchar *language = scan_language_arg(argc, argv);

    flitz_init_i18n(language);
    gtk_init(&argc, &argv);
    initial_path = scan_initial_path(argc, argv);

    app = g_new0(CompressorApp, 1);
    app->ui.window = build_ui(app);
    gtk_widget_show_all(app->ui.window);
    compressor_update_fields(app);

    if (initial_path != NULL) {
        if (g_file_test(initial_path, G_FILE_TEST_IS_DIR)) {
            compressor_set_source(app, initial_path);
        } else if (g_file_test(initial_path, G_FILE_TEST_IS_REGULAR)) {
            gchar *parent = g_path_get_dirname(initial_path);
            compressor_set_source(app, parent);
            g_free(parent);
        }
    }

    gtk_main();

    g_free(initial_path);
    g_free(app->source_path);
    g_free(app->output_dir);
    g_free(app);
    return 0;
}
