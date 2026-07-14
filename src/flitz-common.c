/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "flitz-common.h"

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <limits.h>
#include <locale.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    FlitzUi *ui;
    gchar *text;
} UiTextData;

typedef struct {
    FlitzUi *ui;
    gchar *text;
    gdouble fraction;
    gboolean pulse;
} UiProgressData;

static gboolean append_log_idle(gpointer data)
{
    UiTextData *item = data;
    flitz_log(item->ui, item->text);
    g_free(item->text);
    g_free(item);
    return G_SOURCE_REMOVE;
}

static gboolean progress_idle(gpointer data)
{
    UiProgressData *item = data;
    if (item->pulse) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(item->ui->progress_bar));
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(item->ui->progress_bar),
                                      CLAMP(item->fraction, 0.0, 1.0));
    }
    if (item->text != NULL) {
        gtk_label_set_text(GTK_LABEL(item->ui->progress_label), item->text);
    }
    g_free(item->text);
    g_free(item);
    return G_SOURCE_REMOVE;
}

void flitz_init_i18n(const gchar *language)
{
    setlocale(LC_ALL, "");
    if (language != NULL && *language != '\0') {
        g_setenv("LANGUAGE", language, TRUE);
    }
    bindtextdomain(FLITZ_GETTEXT_PACKAGE, FLITZ_LOCALE_DIR);
    bind_textdomain_codeset(FLITZ_GETTEXT_PACKAGE, "UTF-8");
    textdomain(FLITZ_GETTEXT_PACKAGE);
}

void flitz_set_window_icon(GtkWindow *window)
{
    GError *error = NULL;
    if (g_file_test(FLITZ_ICON, G_FILE_TEST_IS_REGULAR)) {
        gtk_window_set_icon_from_file(window, FLITZ_ICON, &error);
        if (error != NULL) {
            g_warning("Could not load icon %s: %s", FLITZ_ICON, error->message);
            g_clear_error(&error);
        }
    }
}

GtkWidget *flitz_create_logo(gint size)
{
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
        FLITZ_ICON, size, size, TRUE, &error);
    GtkWidget *image;
    if (pixbuf == NULL) {
        g_clear_error(&error);
        return gtk_label_new("Flitz");
    }
    image = gtk_image_new_from_pixbuf(pixbuf);
    g_object_unref(pixbuf);
    return image;
}

void flitz_show_about(GtkWindow *parent, const gchar *program_name)
{
    GtkWidget *dialog;
    GdkPixbuf *logo = NULL;
    GError *error = NULL;
    const gchar *authors[] = { "josejp2424", NULL };

    dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog),
                                      program_name != NULL ? program_name : "Flitz Toolkit");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), FLITZ_VERSION);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                  _("Created by josejp2424 for Essora Linux."));
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog),
                                   _("Copyright © 2026 josejp2424"));
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_GPL_3_0);
    gtk_about_dialog_set_wrap_license(GTK_ABOUT_DIALOG(dialog), TRUE);

    if (g_file_test(FLITZ_ICON, G_FILE_TEST_IS_REGULAR)) {
        logo = gdk_pixbuf_new_from_file_at_scale(FLITZ_ICON, 96, 96, TRUE,
                                                 &error);
        if (logo != NULL) {
            gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), logo);
            g_object_unref(logo);
        } else {
            g_clear_error(&error);
        }
    }

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}


void flitz_ui_init(FlitzUi *ui, GtkWidget *window, GtkWidget *console_view,
                   GtkWidget *progress_bar, GtkWidget *progress_label)
{
    g_return_if_fail(ui != NULL);
    ui->window = window;
    ui->console_view = console_view;
    ui->console_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(console_view));
    ui->progress_bar = progress_bar;
    ui->progress_label = progress_label;
}

void flitz_log(FlitzUi *ui, const gchar *text)
{
    GtkTextIter end;
    g_return_if_fail(ui != NULL && ui->console_buffer != NULL);
    if (text == NULL) {
        return;
    }
    gtk_text_buffer_get_end_iter(ui->console_buffer, &end);
    gtk_text_buffer_insert(ui->console_buffer, &end, text, -1);
    gtk_text_buffer_insert(ui->console_buffer, &end, "\n", 1);
    gtk_text_buffer_get_end_iter(ui->console_buffer, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(ui->console_view), &end,
                                 0.0, FALSE, 0.0, 1.0);
}

void flitz_logf(FlitzUi *ui, const gchar *format, ...)
{
    va_list args;
    gchar *message;
    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);
    flitz_log(ui, message);
    g_free(message);
}

void flitz_queue_log(FlitzUi *ui, const gchar *text)
{
    UiTextData *item = g_new0(UiTextData, 1);
    item->ui = ui;
    item->text = g_strdup(text != NULL ? text : "");
    g_idle_add(append_log_idle, item);
}

void flitz_set_progress(FlitzUi *ui, gdouble fraction, const gchar *text)
{
    g_return_if_fail(ui != NULL);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui->progress_bar),
                                  CLAMP(fraction, 0.0, 1.0));
    if (text != NULL) {
        gtk_label_set_text(GTK_LABEL(ui->progress_label), text);
    }
}

void flitz_queue_progress(FlitzUi *ui, gdouble fraction, const gchar *text,
                          gboolean pulse)
{
    UiProgressData *item = g_new0(UiProgressData, 1);
    item->ui = ui;
    item->fraction = fraction;
    item->pulse = pulse;
    item->text = g_strdup(text);
    g_idle_add(progress_idle, item);
}

void flitz_clear_console(FlitzUi *ui)
{
    g_return_if_fail(ui != NULL && ui->console_buffer != NULL);
    gtk_text_buffer_set_text(ui->console_buffer, "", -1);
}

gboolean flitz_command_exists(const gchar *command)
{
    gchar *path;
    gboolean exists;
    if (command == NULL || *command == '\0') {
        return FALSE;
    }
    if (g_path_is_absolute(command)) {
        return g_file_test(command, G_FILE_TEST_IS_EXECUTABLE);
    }
    path = g_find_program_in_path(command);
    exists = path != NULL;
    g_free(path);
    return exists;
}

static void emit_accumulated_line(GString *line, FlitzLineFunc line_func,
                                  gpointer user_data)
{
    if (line->len == 0) {
        return;
    }
    g_strstrip(line->str);
    if (line->str[0] != '\0' && line_func != NULL) {
        gchar *valid = g_utf8_make_valid(line->str, -1);
        line_func(valid, user_data);
        g_free(valid);
    }
    g_string_set_size(line, 0);
}

gboolean flitz_run_command(const gchar *const argv[], const gchar *cwd,
                           FlitzLineFunc line_func, gpointer user_data,
                           GError **error)
{
    GSubprocessLauncher *launcher;
    GSubprocess *process;
    GInputStream *stream;
    GString *line;
    gchar buffer[4096];
    gssize count;
    gboolean ok = FALSE;
    GError *local_error = NULL;

    g_return_val_if_fail(argv != NULL && argv[0] != NULL, FALSE);

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE);
    if (cwd != NULL && *cwd != '\0') {
        g_subprocess_launcher_set_cwd(launcher, cwd);
    }

    process = g_subprocess_launcher_spawnv(launcher, argv, &local_error);
    g_object_unref(launcher);
    if (process == NULL) {
        g_propagate_error(error, local_error);
        return FALSE;
    }

    stream = g_subprocess_get_stdout_pipe(process);
    line = g_string_new(NULL);
    while ((count = g_input_stream_read(stream, buffer, sizeof(buffer), NULL,
                                        &local_error)) > 0) {
        gssize i;
        for (i = 0; i < count; i++) {
            if (buffer[i] == '\n' || buffer[i] == '\r') {
                emit_accumulated_line(line, line_func, user_data);
            } else {
                g_string_append_c(line, buffer[i]);
            }
        }
    }
    emit_accumulated_line(line, line_func, user_data);
    g_string_free(line, TRUE);

    if (count < 0) {
        g_object_unref(process);
        g_propagate_error(error, local_error);
        return FALSE;
    }

    ok = g_subprocess_wait_check(process, NULL, &local_error);
    g_object_unref(process);
    if (!ok) {
        g_propagate_error(error, local_error);
        return FALSE;
    }
    return TRUE;
}

gboolean flitz_run_command_quiet(const gchar *const argv[], const gchar *cwd)
{
    GSubprocessLauncher *launcher;
    GSubprocess *process;
    gboolean ok;
    GError *error = NULL;

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                         G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    if (cwd != NULL && *cwd != '\0') {
        g_subprocess_launcher_set_cwd(launcher, cwd);
    }
    process = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher);
    if (process == NULL) {
        g_clear_error(&error);
        return FALSE;
    }
    ok = g_subprocess_wait_check(process, NULL, &error);
    g_object_unref(process);
    g_clear_error(&error);
    return ok;
}

gchar *flitz_first_dropped_path(GtkSelectionData *selection_data)
{
    gchar **uris;
    gchar *path = NULL;
    const guchar *raw;

    if (selection_data == NULL) {
        return NULL;
    }
    uris = gtk_selection_data_get_uris(selection_data);
    if (uris != NULL && uris[0] != NULL) {
        path = g_filename_from_uri(uris[0], NULL, NULL);
        g_strfreev(uris);
        return path;
    }

    raw = gtk_selection_data_get_data(selection_data);
    if (raw != NULL) {
        gchar *text = g_strstrip(g_strdup((const gchar *)raw));
        if (g_str_has_prefix(text, "file://")) {
            path = g_filename_from_uri(text, NULL, NULL);
        } else if (*text != '\0') {
            path = g_strdup(text);
        }
        g_free(text);
    }
    return path;
}

gchar *flitz_path_dirname(const gchar *path)
{
    if (path == NULL || *path == '\0') {
        return g_get_current_dir();
    }
    return g_path_get_dirname(path);
}

gchar *flitz_path_basename(const gchar *path)
{
    if (path == NULL) {
        return g_strdup("");
    }
    return g_path_get_basename(path);
}

static gboolean ends_with_ci(const gchar *text, const gchar *suffix)
{
    gchar *lower_text;
    gchar *lower_suffix;
    gboolean result;
    lower_text = g_utf8_strdown(text, -1);
    lower_suffix = g_utf8_strdown(suffix, -1);
    result = g_str_has_suffix(lower_text, lower_suffix);
    g_free(lower_text);
    g_free(lower_suffix);
    return result;
}

gchar *flitz_archive_extension(const gchar *path)
{
    static const gchar *multi[] = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz4",
        ".tar.7z", ".tar.zip", NULL
    };
    gchar *base = g_path_get_basename(path != NULL ? path : "");
    gchar *lower = g_utf8_strdown(base, -1);
    gchar *result = NULL;
    const gchar *dot;
    guint i;

    for (i = 0; multi[i] != NULL; i++) {
        if (g_str_has_suffix(lower, multi[i])) {
            result = g_strdup(multi[i] + 1);
            break;
        }
    }
    if (result == NULL && g_str_has_suffix(lower, ".squashfs")) {
        result = g_strdup("sfs");
    }
    if (result == NULL) {
        dot = strrchr(lower, '.');
        result = dot != NULL ? g_strdup(dot + 1) : g_strdup("");
    }
    g_free(lower);
    g_free(base);
    return result;
}

gchar *flitz_archive_basename(const gchar *path)
{
    static const gchar *suffixes[] = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz4",
        ".tar.7z", ".tar.zip", ".squashfs", ".appimage", ".zip",
        ".tar", ".tgz", ".tbz2", ".txz", ".7z", ".rar", ".pet",
        ".deb", ".rpm", ".dmg", ".iso", ".exe", ".msi", ".cab",
        ".sfs", ".zst", ".gz", ".bz2", ".xz", NULL
    };
    gchar *base = g_path_get_basename(path != NULL ? path : "");
    guint i;
    for (i = 0; suffixes[i] != NULL; i++) {
        if (ends_with_ci(base, suffixes[i])) {
            gsize len = strlen(base) - strlen(suffixes[i]);
            gchar *result = g_strndup(base, len);
            g_free(base);
            if (result[0] == '\0') {
                g_free(result);
                return g_strdup("archive");
            }
            return result;
        }
    }
    return base;
}

gboolean flitz_is_supported_archive(const gchar *path)
{
    static const gchar *extensions[] = {
        "zip", "tar", "tar.gz", "tar.bz2", "tar.xz", "tar.zst",
        "tgz", "tbz2", "txz", "gz", "bz2", "xz", "7z", "rar",
        "pet", "deb", "rpm", "dmg", "iso", "exe", "msi", "cab",
        "appimage", "sfs", "zst", "001", "r00", NULL
    };
    gchar *ext = flitz_archive_extension(path);
    guint i;
    gboolean found = FALSE;
    for (i = 0; extensions[i] != NULL; i++) {
        if (g_ascii_strcasecmp(ext, extensions[i]) == 0) {
            found = TRUE;
            break;
        }
    }
    g_free(ext);
    return found;
}

gboolean flitz_ensure_directory(const gchar *path, GError **error)
{
    if (path == NULL || *path == '\0') {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                    "Invalid empty directory path");
        return FALSE;
    }
    if (g_mkdir_with_parents(path, 0755) == 0) {
        return TRUE;
    }
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Cannot create directory %s: %s", path, g_strerror(errno));
    return FALSE;
}

gboolean flitz_remove_tree(const gchar *path, GError **error)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return TRUE;
        }
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot inspect %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        if (dir == NULL) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot open directory %s: %s", path, g_strerror(errno));
            return FALSE;
        }
        while ((entry = readdir(dir)) != NULL) {
            gchar *child;
            if (g_strcmp0(entry->d_name, ".") == 0 ||
                g_strcmp0(entry->d_name, "..") == 0) {
                continue;
            }
            child = g_build_filename(path, entry->d_name, NULL);
            if (!flitz_remove_tree(child, error)) {
                g_free(child);
                closedir(dir);
                return FALSE;
            }
            g_free(child);
        }
        closedir(dir);
        if (rmdir(path) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot remove directory %s: %s", path, g_strerror(errno));
            return FALSE;
        }
    } else if (unlink(path) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot remove %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    return TRUE;
}

static gboolean destination_exists(const gchar *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static gboolean prepare_destination(const gchar *dst, gboolean overwrite,
                                    gboolean *skip, GError **error)
{
    *skip = FALSE;
    if (!destination_exists(dst)) {
        return TRUE;
    }
    if (!overwrite) {
        *skip = TRUE;
        return TRUE;
    }
    return flitz_remove_tree(dst, error);
}

static gboolean copy_regular_file(const gchar *src, const gchar *dst,
                                  mode_t mode, gboolean overwrite,
                                  gboolean *skipped, GError **error)
{
    int in_fd = -1;
    int out_fd = -1;
    gchar buffer[65536];
    ssize_t got;
    gboolean skip;
    gchar *parent;

    if (!prepare_destination(dst, overwrite, &skip, error)) {
        return FALSE;
    }
    if (skip) {
        *skipped = TRUE;
        return TRUE;
    }
    parent = g_path_get_dirname(dst);
    if (!flitz_ensure_directory(parent, error)) {
        g_free(parent);
        return FALSE;
    }
    g_free(parent);

    in_fd = open(src, O_RDONLY | O_CLOEXEC);
    if (in_fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot open %s: %s", src, g_strerror(errno));
        return FALSE;
    }
    out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  mode & 07777);
    if (out_fd < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot create %s: %s", dst, g_strerror(errno));
        close(in_fd);
        return FALSE;
    }
    while ((got = read(in_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < got) {
            ssize_t written = write(out_fd, buffer + offset,
                                    (size_t)(got - offset));
            if (written < 0) {
                g_set_error(error, G_FILE_ERROR,
                            g_file_error_from_errno(errno),
                            "Cannot write %s: %s", dst, g_strerror(errno));
                close(in_fd);
                close(out_fd);
                return FALSE;
            }
            offset += written;
        }
    }
    if (got < 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot read %s: %s", src, g_strerror(errno));
        close(in_fd);
        close(out_fd);
        return FALSE;
    }
    close(in_fd);
    if (close(out_fd) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot close %s: %s", dst, g_strerror(errno));
        return FALSE;
    }
    *skipped = FALSE;
    return TRUE;
}

static gboolean copy_entry_recursive(const gchar *src, const gchar *dst,
                                     gboolean overwrite,
                                     FlitzLineFunc line_func,
                                     gpointer user_data, GError **error)
{
    struct stat st;
    gboolean skip = FALSE;

    if (lstat(src, &st) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot inspect %s: %s", src, g_strerror(errno));
        return FALSE;
    }

    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *dir;
        struct dirent *entry;
        if (destination_exists(dst)) {
            struct stat dst_st;
            if (lstat(dst, &dst_st) != 0) {
                g_set_error(error, G_FILE_ERROR,
                            g_file_error_from_errno(errno),
                            "Cannot inspect %s: %s", dst, g_strerror(errno));
                return FALSE;
            }
            if (!S_ISDIR(dst_st.st_mode) || S_ISLNK(dst_st.st_mode)) {
                if (!overwrite) {
                    if (line_func != NULL) {
                        gchar *msg = g_strdup_printf(_("Skipped existing: %s"), dst);
                        line_func(msg, user_data);
                        g_free(msg);
                    }
                    return TRUE;
                }
                if (!flitz_remove_tree(dst, error)) {
                    return FALSE;
                }
            }
        }
        if (g_mkdir_with_parents(dst, st.st_mode & 07777) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot create directory %s: %s", dst,
                        g_strerror(errno));
            return FALSE;
        }
        dir = opendir(src);
        if (dir == NULL) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot open directory %s: %s", src,
                        g_strerror(errno));
            return FALSE;
        }
        while ((entry = readdir(dir)) != NULL) {
            gchar *child_src;
            gchar *child_dst;
            if (g_strcmp0(entry->d_name, ".") == 0 ||
                g_strcmp0(entry->d_name, "..") == 0) {
                continue;
            }
            child_src = g_build_filename(src, entry->d_name, NULL);
            child_dst = g_build_filename(dst, entry->d_name, NULL);
            if (!copy_entry_recursive(child_src, child_dst, overwrite,
                                      line_func, user_data, error)) {
                g_free(child_src);
                g_free(child_dst);
                closedir(dir);
                return FALSE;
            }
            g_free(child_src);
            g_free(child_dst);
        }
        closedir(dir);
        return TRUE;
    }

    if (S_ISLNK(st.st_mode)) {
        gchar target[PATH_MAX + 1];
        ssize_t length;
        gchar *parent;
        if (!prepare_destination(dst, overwrite, &skip, error)) {
            return FALSE;
        }
        if (skip) {
            if (line_func != NULL) {
                gchar *msg = g_strdup_printf(_("Skipped existing: %s"), dst);
                line_func(msg, user_data);
                g_free(msg);
            }
            return TRUE;
        }
        length = readlink(src, target, PATH_MAX);
        if (length < 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot read symbolic link %s: %s", src,
                        g_strerror(errno));
            return FALSE;
        }
        target[length] = '\0';
        parent = g_path_get_dirname(dst);
        if (!flitz_ensure_directory(parent, error)) {
            g_free(parent);
            return FALSE;
        }
        g_free(parent);
        if (symlink(target, dst) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot create symbolic link %s: %s", dst,
                        g_strerror(errno));
            return FALSE;
        }
        return TRUE;
    }

    if (S_ISREG(st.st_mode)) {
        gboolean skipped = FALSE;
        if (!copy_regular_file(src, dst, st.st_mode, overwrite, &skipped,
                               error)) {
            return FALSE;
        }
        if (line_func != NULL) {
            gchar *msg = skipped
                ? g_strdup_printf(_("Skipped existing: %s"), dst)
                : g_strdup_printf(_("Extracted: %s"), dst);
            line_func(msg, user_data);
            g_free(msg);
        }
        return TRUE;
    }

    if (line_func != NULL) {
        gchar *msg = g_strdup_printf(_("Skipped unsupported special file: %s"), src);
        line_func(msg, user_data);
        g_free(msg);
    }
    return TRUE;
}

static gboolean flatten_recursive(const gchar *src_dir, const gchar *output_dir,
                                  gboolean overwrite,
                                  FlitzLineFunc line_func,
                                  gpointer user_data, GError **error)
{
    DIR *dir = opendir(src_dir);
    struct dirent *entry;
    if (dir == NULL) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot open directory %s: %s", src_dir,
                    g_strerror(errno));
        return FALSE;
    }
    while ((entry = readdir(dir)) != NULL) {
        gchar *src;
        struct stat st;
        if (g_strcmp0(entry->d_name, ".") == 0 ||
            g_strcmp0(entry->d_name, "..") == 0) {
            continue;
        }
        src = g_build_filename(src_dir, entry->d_name, NULL);
        if (lstat(src, &st) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot inspect %s: %s", src, g_strerror(errno));
            g_free(src);
            closedir(dir);
            return FALSE;
        }
        if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
            if (!flatten_recursive(src, output_dir, overwrite, line_func,
                                   user_data, error)) {
                g_free(src);
                closedir(dir);
                return FALSE;
            }
        } else {
            gchar *dst = g_build_filename(output_dir, entry->d_name, NULL);
            if (!copy_entry_recursive(src, dst, overwrite, line_func,
                                      user_data, error)) {
                g_free(src);
                g_free(dst);
                closedir(dir);
                return FALSE;
            }
            g_free(dst);
        }
        g_free(src);
    }
    closedir(dir);
    return TRUE;
}

gboolean flitz_commit_stage(const gchar *stage_dir, const gchar *output_dir,
                            gboolean keep_structure, gboolean overwrite,
                            FlitzLineFunc line_func, gpointer user_data,
                            GError **error)
{
    DIR *dir;
    struct dirent *entry;
    if (!flitz_ensure_directory(output_dir, error)) {
        return FALSE;
    }
    if (!keep_structure) {
        return flatten_recursive(stage_dir, output_dir, overwrite, line_func,
                                 user_data, error);
    }
    dir = opendir(stage_dir);
    if (dir == NULL) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot open directory %s: %s", stage_dir,
                    g_strerror(errno));
        return FALSE;
    }
    while ((entry = readdir(dir)) != NULL) {
        gchar *src;
        gchar *dst;
        if (g_strcmp0(entry->d_name, ".") == 0 ||
            g_strcmp0(entry->d_name, "..") == 0) {
            continue;
        }
        src = g_build_filename(stage_dir, entry->d_name, NULL);
        dst = g_build_filename(output_dir, entry->d_name, NULL);
        if (!copy_entry_recursive(src, dst, overwrite, line_func, user_data,
                                  error)) {
            g_free(src);
            g_free(dst);
            closedir(dir);
            return FALSE;
        }
        g_free(src);
        g_free(dst);
    }
    closedir(dir);
    return TRUE;
}

guint64 flitz_count_entries(const gchar *path)
{
    struct stat st;
    guint64 total = 0;
    if (lstat(path, &st) != 0) {
        return 0;
    }
    total = 1;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        if (dir == NULL) {
            return total;
        }
        while ((entry = readdir(dir)) != NULL) {
            gchar *child;
            if (g_strcmp0(entry->d_name, ".") == 0 ||
                g_strcmp0(entry->d_name, "..") == 0) {
                continue;
            }
            child = g_build_filename(path, entry->d_name, NULL);
            total += flitz_count_entries(child);
            g_free(child);
        }
        closedir(dir);
    }
    return total;
}

gchar *flitz_short_path(const gchar *path, gsize max_chars)
{
    gsize length;
    const gchar *start;
    if (path == NULL) {
        return g_strdup("");
    }
    length = g_utf8_strlen(path, -1);
    if (length <= max_chars || max_chars < 4) {
        return g_strdup(path);
    }
    start = g_utf8_offset_to_pointer(path, (glong)(length - (max_chars - 3)));
    return g_strconcat("...", start, NULL);
}

gint flitz_parse_percent(const gchar *line)
{
    const gchar *percent;
    const gchar *start;
    gchar *number;
    gint value;
    if (line == NULL) {
        return -1;
    }
    percent = strchr(line, '%');
    if (percent == NULL) {
        return -1;
    }
    start = percent;
    while (start > line && g_ascii_isdigit((guchar)start[-1])) {
        start--;
    }
    if (start == percent) {
        return -1;
    }
    number = g_strndup(start, (gsize)(percent - start));
    value = (gint)g_ascii_strtoll(number, NULL, 10);
    g_free(number);
    return (value >= 0 && value <= 100) ? value : -1;
}
