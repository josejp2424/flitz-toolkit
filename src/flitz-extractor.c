/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "flitz-common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PET_FOOTER_SIZE 32

typedef struct {
    FlitzUi ui;
    GtkWidget *file_label;
    GtkWidget *output_label;
    GtkWidget *recursive_check;
    GtkWidget *keep_check;
    GtkWidget *overwrite_check;
    GtkWidget *extract_button;
    gchar *file_path;
    gchar *output_dir;
    gboolean running;
    gboolean auto_start;
} ExtractorApp;

typedef struct {
    ExtractorApp *app;
    gchar *file_path;
    gchar *output_dir;
    gboolean recursive;
    gboolean keep_structure;
    gboolean overwrite;
} ExtractJob;

typedef struct {
    ExtractorApp *app;
    gboolean success;
    gchar *message;
} ExtractFinish;

typedef struct {
    ExtractorApp *app;
    guint64 seen_lines;
} ExtractLineState;

static void extractor_set_file(ExtractorApp *app, const gchar *path);
static void extractor_set_output(ExtractorApp *app, const gchar *path);
static gboolean extractor_choose_output(ExtractorApp *app);
static void extractor_start(ExtractorApp *app);
static gboolean extract_archive_to_stage(ExtractJob *job, const gchar *archive,
                                         const gchar *stage, GError **error);

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

static void extract_line(const gchar *line, gpointer user_data)
{
    ExtractLineState *state = user_data;
    gint percent = flitz_parse_percent(line);
    state->seen_lines++;
    flitz_queue_log(&state->app->ui, line);
    if (percent >= 0) {
        gchar *label = g_strdup_printf(_("%d%% completed"), percent);
        flitz_queue_progress(&state->app->ui, percent / 100.0, label, FALSE);
        g_free(label);
    } else {
        flitz_queue_progress(&state->app->ui, 0.0, _("Extracting..."), TRUE);
    }
}

static gboolean run_logged(ExtractJob *job, const gchar *const argv[],
                           const gchar *cwd, GError **error)
{
    ExtractLineState state = { job->app, 0 };
    GString *command = g_string_new(NULL);
    guint i;
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
    return flitz_run_command(argv, cwd, extract_line, &state, error);
}

static gboolean command_required(const gchar *command, GError **error)
{
    if (flitz_command_exists(command)) {
        return TRUE;
    }
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                _("Required tool is not installed: %s"), command);
    return FALSE;
}

static gboolean extract_tar(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    const gchar *argv[] = { "tar", "-xvf", archive, "-C", stage, NULL };
    return command_required("tar", error) &&
           run_logged(job, argv, NULL, error);
}

static gboolean pet_footer_is_md5(const gchar footer[PET_FOOTER_SIZE + 1])
{
    guint i;
    for (i = 0; i < PET_FOOTER_SIZE; i++) {
        const gchar c = footer[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean read_exact_fd(gint fd, void *buffer, gsize length,
                              GError **error)
{
    gsize done = 0;
    while (done < length) {
        ssize_t count = read(fd, (gchar *)buffer + done, length - done);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        _("Cannot read PET package: %s"), g_strerror(errno));
            return FALSE;
        }
        if (count == 0) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                        "%s", _("PET package ended unexpectedly."));
            return FALSE;
        }
        done += (gsize)count;
    }
    return TRUE;
}

static gboolean write_all_fd(gint fd, const void *buffer, gsize length,
                             GError **error)
{
    gsize done = 0;
    while (done < length) {
        ssize_t count = write(fd, (const gchar *)buffer + done,
                              length - done);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        _("Cannot write temporary PET archive: %s"),
                        g_strerror(errno));
            return FALSE;
        }
        done += (gsize)count;
    }
    return TRUE;
}

static gboolean extract_pet(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    gint source_fd = -1;
    gint payload_fd = -1;
    struct stat st;
    gchar footer[PET_FOOTER_SIZE + 1];
    gchar buffer[65536];
    gchar *payload_path = NULL;
    GChecksum *checksum = NULL;
    off_t remaining;
    gboolean ok = FALSE;

    source_fd = open(archive, O_RDONLY);
    if (source_fd < 0 || fstat(source_fd, &st) != 0) {
        gint saved_errno = errno;
        if (source_fd >= 0) {
            close(source_fd);
        }
        g_set_error(error, G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    _("Cannot inspect %s: %s"), archive,
                    g_strerror(saved_errno));
        return FALSE;
    }

    /*
     * Puppy PET packages are compressed tar archives followed by a raw
     * 32-character MD5 footer. Passing that footer to xz/gzip makes tar
     * extract the files and then report a false corruption error. This is
     * the same footer handled by Puppy's pet2tgz utility.
     */
    if (st.st_size <= PET_FOOTER_SIZE ||
        lseek(source_fd, st.st_size - PET_FOOTER_SIZE, SEEK_SET) < 0 ||
        !read_exact_fd(source_fd, footer, PET_FOOTER_SIZE, error)) {
        close(source_fd);
        if (error != NULL && *error != NULL) {
            return FALSE;
        }
        flitz_queue_log(&job->app->ui,
                        _("Legacy PET without checksum footer; extracting directly."));
        return extract_tar(job, archive, stage, error);
    }
    footer[PET_FOOTER_SIZE] = '\0';

    if (!pet_footer_is_md5(footer)) {
        close(source_fd);
        flitz_queue_log(&job->app->ui,
                        _("Legacy PET without checksum footer; extracting directly."));
        return extract_tar(job, archive, stage, error);
    }

    if (lseek(source_fd, 0, SEEK_SET) < 0) {
        gint saved_errno = errno;
        close(source_fd);
        g_set_error(error, G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    _("Cannot read PET package: %s"),
                    g_strerror(saved_errno));
        return FALSE;
    }

    payload_path = g_strdup("/tmp/flitz-pet-XXXXXX");
    payload_fd = mkstemp(payload_path);
    if (payload_fd < 0) {
        gint saved_errno = errno;
        close(source_fd);
        g_set_error(error, G_FILE_ERROR,
                    g_file_error_from_errno(saved_errno),
                    _("Cannot create temporary PET archive: %s"),
                    g_strerror(saved_errno));
        g_free(payload_path);
        return FALSE;
    }

    checksum = g_checksum_new(G_CHECKSUM_MD5);
    remaining = st.st_size - PET_FOOTER_SIZE;
    while (remaining > 0) {
        gsize wanted = (gsize)MIN((off_t)sizeof(buffer), remaining);
        ssize_t count = read(source_fd, buffer, wanted);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_set_error(error, G_FILE_ERROR,
                        g_file_error_from_errno(errno),
                        _("Cannot read PET package: %s"),
                        g_strerror(errno));
            goto cleanup;
        }
        if (count == 0) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                        "%s", _("PET package ended unexpectedly."));
            goto cleanup;
        }
        g_checksum_update(checksum, (const guchar *)buffer, (gsize)count);
        if (!write_all_fd(payload_fd, buffer, (gsize)count, error)) {
            goto cleanup;
        }
        remaining -= count;
    }

    if (g_ascii_strcasecmp(g_checksum_get_string(checksum), footer) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s",
                    _("PET checksum does not match. The package may be damaged."));
        goto cleanup;
    }
    flitz_queue_log(&job->app->ui, _("PET checksum verified."));

    if (close(payload_fd) != 0) {
        payload_fd = -1;
        g_set_error(error, G_FILE_ERROR,
                    g_file_error_from_errno(errno),
                    _("Cannot write temporary PET archive: %s"),
                    g_strerror(errno));
        goto cleanup;
    }
    payload_fd = -1;
    close(source_fd);
    source_fd = -1;

    if (command_required("tar", error)) {
        const gchar *argv[] = { "tar", "-xvf", payload_path,
                                "-C", stage, NULL };
        ok = run_logged(job, argv, NULL, error);
    }

cleanup:
    if (checksum != NULL) {
        g_checksum_free(checksum);
    }
    if (source_fd >= 0) {
        close(source_fd);
    }
    if (payload_fd >= 0) {
        close(payload_fd);
    }
    if (payload_path != NULL) {
        g_unlink(payload_path);
        g_free(payload_path);
    }
    return ok;
}

static gboolean extract_zip(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    if (flitz_command_exists("unzip")) {
        const gchar *argv[] = { "unzip", "-o", archive, "-d", stage, NULL };
        return run_logged(job, argv, NULL, error);
    }
    if (flitz_command_exists("7z")) {
        gchar *out_arg = g_strconcat("-o", stage, NULL);
        const gchar *argv[] = { "7z", "x", "-y", archive, out_arg, NULL };
        gboolean ok = run_logged(job, argv, NULL, error);
        g_free(out_arg);
        return ok;
    }
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                "%s", _("Neither unzip nor 7z is installed."));
    return FALSE;
}

static gboolean expand_single_nested_from_7z(ExtractJob *job,
                                                const gchar *stage,
                                                GError **error)
{
    GDir *dir = g_dir_open(stage, 0, error);
    const gchar *entry;
    gchar *single = NULL;
    guint count = 0;
    gboolean ok = TRUE;

    if (dir == NULL) {
        return FALSE;
    }
    while ((entry = g_dir_read_name(dir)) != NULL) {
        count++;
        if (count == 1) {
            single = g_build_filename(stage, entry, NULL);
        } else {
            break;
        }
    }
    g_dir_close(dir);

    if (count == 1 && single != NULL &&
        g_file_test(single, G_FILE_TEST_IS_REGULAR)) {
        gchar *ext = flitz_archive_extension(single);
        if (g_ascii_strcasecmp(ext, "tar") == 0) {
            const gchar *argv[] = { "tar", "-xvf", single, "-C", stage,
                                    NULL };
            ok = command_required("tar", error) &&
                 run_logged(job, argv, NULL, error);
            if (ok) {
                g_unlink(single);
            }
        } else if (g_ascii_strcasecmp(ext, "zip") == 0) {
            if (flitz_command_exists("unzip")) {
                const gchar *argv[] = { "unzip", "-o", single, "-d", stage,
                                        NULL };
                ok = run_logged(job, argv, NULL, error);
            } else {
                gchar *out_arg = g_strconcat("-o", stage, NULL);
                const gchar *argv[] = { "7z", "x", "-y", single, out_arg,
                                        NULL };
                ok = run_logged(job, argv, NULL, error);
                g_free(out_arg);
            }
            if (ok) {
                g_unlink(single);
            }
        }
        g_free(ext);
    }
    g_free(single);
    return ok;
}

static gboolean extract_7z_generic(ExtractJob *job, const gchar *archive,
                                   const gchar *stage, GError **error)
{
    gchar *out_arg;
    gboolean ok;
    const gchar *argv[6];
    if (!command_required("7z", error)) {
        return FALSE;
    }
    out_arg = g_strconcat("-o", stage, NULL);
    argv[0] = "7z";
    argv[1] = "x";
    argv[2] = "-y";
    argv[3] = archive;
    argv[4] = out_arg;
    argv[5] = NULL;
    ok = run_logged(job, argv, NULL, error);
    g_free(out_arg);
    if (ok) {
        ok = expand_single_nested_from_7z(job, stage, error);
    }
    return ok;
}

static gboolean extract_rar(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    if (flitz_command_exists("unrar")) {
        gchar *destination = g_strconcat(stage, G_DIR_SEPARATOR_S, NULL);
        const gchar *argv[] = { "unrar", "x", "-y", "-o+", archive,
                                destination, NULL };
        gboolean ok = run_logged(job, argv, NULL, error);
        g_free(destination);
        return ok;
    }
    return extract_7z_generic(job, archive, stage, error);
}

static gboolean extract_deb(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    gchar *name = flitz_archive_basename(archive);
    gchar *underscore = strchr(name, '_');
    gchar *destination;
    gboolean ok = FALSE;
    if (underscore != NULL) {
        *underscore = '\0';
    }
    destination = g_build_filename(stage, name[0] != '\0' ? name : "deb-package", NULL);
    g_free(name);
    if (!flitz_ensure_directory(destination, error)) {
        g_free(destination);
        return FALSE;
    }
    if (flitz_command_exists("dpkg-deb")) {
        const gchar *argv[] = { "dpkg-deb", "-x", archive, destination, NULL };
        ok = run_logged(job, argv, NULL, error);
    } else if (flitz_command_exists("ar") && flitz_command_exists("tar")) {
        gchar *temporary = g_dir_make_tmp("flitz-deb-XXXXXX", error);
        if (temporary != NULL) {
            const gchar *ar_argv[] = { "ar", "x", archive, NULL };
            if (run_logged(job, ar_argv, temporary, error)) {
                GDir *dir = g_dir_open(temporary, 0, error);
                const gchar *entry;
                gchar *data_tar = NULL;
                if (dir != NULL) {
                    while ((entry = g_dir_read_name(dir)) != NULL) {
                        if (g_str_has_prefix(entry, "data.tar")) {
                            data_tar = g_build_filename(temporary, entry, NULL);
                            break;
                        }
                    }
                    g_dir_close(dir);
                }
                if (data_tar != NULL) {
                    const gchar *tar_argv[] = { "tar", "-xvf", data_tar,
                                                "-C", destination, NULL };
                    ok = run_logged(job, tar_argv, NULL, error);
                    g_free(data_tar);
                } else if (error == NULL || *error == NULL) {
                    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                                "%s", _("The DEB package has no data archive."));
                }
            }
            {
                GError *cleanup_error = NULL;
                flitz_remove_tree(temporary, &cleanup_error);
                g_clear_error(&cleanup_error);
            }
            g_free(temporary);
        }
    } else {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "%s", _("Install dpkg-deb, or install ar and tar."));
    }
    g_free(destination);
    return ok;
}

static gchar *rpm_simple_name(const gchar *archive)
{
    gchar *base = flitz_archive_basename(archive);
    gchar *dash = strchr(base, '-');
    if (dash != NULL) {
        *dash = '\0';
    }
    if (*base == '\0') {
        g_free(base);
        return g_strdup("rpm-package");
    }
    return base;
}

static gboolean extract_rpm(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    gchar *name;
    gchar *destination;
    gchar *q_archive;
    gchar *q_destination;
    gchar *script;
    gboolean ok;
    const gchar *argv[4];

    if (!command_required("rpm2cpio", error) ||
        !command_required("cpio", error)) {
        return FALSE;
    }
    name = rpm_simple_name(archive);
    destination = g_build_filename(stage, name, NULL);
    g_free(name);
    if (!flitz_ensure_directory(destination, error)) {
        g_free(destination);
        return FALSE;
    }
    q_archive = g_shell_quote(archive);
    q_destination = g_shell_quote(destination);
    script = g_strdup_printf("rpm2cpio %s | (cd %s && cpio -idmu)",
                             q_archive, q_destination);
    g_free(q_archive);
    g_free(q_destination);
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = script;
    argv[3] = NULL;
    ok = run_logged(job, argv, NULL, error);
    g_free(script);
    g_free(destination);
    return ok;
}

static gboolean extract_dmg(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    gchar *image;
    gboolean ok;
    if (!command_required("dmg2img", error) ||
        !command_required("7z", error)) {
        return FALSE;
    }
    image = g_build_filename(stage, ".flitz-dmg.img", NULL);
    {
        const gchar *convert_argv[] = { "dmg2img", archive, image, NULL };
        ok = run_logged(job, convert_argv, NULL, error);
    }
    if (ok) {
        gchar *out_arg = g_strconcat("-o", stage, NULL);
        const gchar *extract_argv[] = { "7z", "x", "-y", image, out_arg,
                                        NULL };
        ok = run_logged(job, extract_argv, NULL, error);
        g_free(out_arg);
    }
    g_unlink(image);
    g_free(image);
    return ok;
}

static gboolean extract_exe(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    if (flitz_command_exists("innoextract")) {
        GError *first_error = NULL;
        const gchar *argv[] = { "innoextract", "-d", stage, archive, NULL };
        if (run_logged(job, argv, NULL, &first_error)) {
            return TRUE;
        }
        flitz_queue_log(&job->app->ui,
                        _("innoextract could not extract it; trying 7z."));
        g_clear_error(&first_error);
    }
    return extract_7z_generic(job, archive, stage, error);
}

static gboolean extract_msi(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    const gchar *argv[] = { "msiextract", "--directory", stage, archive,
                            NULL };
    return command_required("msiextract", error) &&
           run_logged(job, argv, NULL, error);
}

static gboolean extract_cab(ExtractJob *job, const gchar *archive,
                            const gchar *stage, GError **error)
{
    const gchar *argv[] = { "cabextract", "-d", stage, archive, NULL };
    return command_required("cabextract", error) &&
           run_logged(job, argv, NULL, error);
}

static gboolean extract_appimage(ExtractJob *job, const gchar *archive,
                                 const gchar *stage, GError **error)
{
    struct stat st;
    const gchar *argv[] = { archive, "--appimage-extract", NULL };
    if (stat(archive, &st) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    _("Cannot inspect %s: %s"), archive, g_strerror(errno));
        return FALSE;
    }
    if (chmod(archive, st.st_mode | S_IXUSR) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    _("Cannot make AppImage executable: %s"),
                    g_strerror(errno));
        return FALSE;
    }
    return run_logged(job, argv, stage, error);
}

static gboolean extract_squashfs(ExtractJob *job, const gchar *archive,
                                 const gchar *stage, GError **error)
{
    gchar *name = flitz_archive_basename(archive);
    gchar *destination = g_build_filename(stage, name, NULL);
    const gchar *argv[] = { "unsquashfs", "-f", "-d", destination,
                            archive, NULL };
    gboolean ok = command_required("unsquashfs", error) &&
                  run_logged(job, argv, NULL, error);
    g_free(name);
    g_free(destination);
    return ok;
}

static gboolean extract_single_compressed(ExtractJob *job,
                                          const gchar *archive,
                                          const gchar *stage,
                                          const gchar *program,
                                          GError **error)
{
    gchar *base = g_path_get_basename(archive);
    gchar *dot = strrchr(base, '.');
    gchar *destination;
    gchar *q_archive;
    gchar *q_destination;
    gchar *script;
    const gchar *argv[4];
    gboolean ok;
    if (dot != NULL) {
        *dot = '\0';
    }
    destination = g_build_filename(stage, base, NULL);
    g_free(base);
    if (!command_required(program, error)) {
        g_free(destination);
        return FALSE;
    }
    q_archive = g_shell_quote(archive);
    q_destination = g_shell_quote(destination);
    script = g_strdup_printf("%s -dc -- %s > %s", program, q_archive,
                             q_destination);
    g_free(q_archive);
    g_free(q_destination);
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = script;
    argv[3] = NULL;
    ok = run_logged(job, argv, NULL, error);
    g_free(script);
    g_free(destination);
    return ok;
}

static gboolean extract_zstd(ExtractJob *job, const gchar *archive,
                             const gchar *stage, GError **error)
{
    gchar *name;
    gchar *destination_dir;
    gchar *temporary;
    gboolean ok;
    if (!command_required("zstd", error)) {
        return FALSE;
    }
    name = flitz_archive_basename(archive);
    destination_dir = g_build_filename(stage, name, NULL);
    temporary = g_build_filename(stage, ".flitz-zstd-output", NULL);
    {
        const gchar *argv[] = { "zstd", "-d", "-f", archive, "-o",
                                temporary, NULL };
        ok = run_logged(job, argv, NULL, error);
    }
    if (ok) {
        const gchar *test_argv[] = { "tar", "-tf", temporary, NULL };
        if (flitz_command_exists("tar") &&
            flitz_run_command_quiet(test_argv, NULL)) {
            const gchar *tar_argv[] = { "tar", "-xvf", temporary, "-C",
                                        destination_dir, NULL };
            ok = flitz_ensure_directory(destination_dir, error) &&
                 run_logged(job, tar_argv, NULL, error);
        } else {
            gchar *target;
            if (!flitz_ensure_directory(destination_dir, error)) {
                ok = FALSE;
            } else {
                target = g_build_filename(destination_dir, name, NULL);
                if (g_rename(temporary, target) != 0) {
                    g_set_error(error, G_FILE_ERROR,
                                g_file_error_from_errno(errno),
                                _("Cannot move decompressed file: %s"),
                                g_strerror(errno));
                    ok = FALSE;
                }
                g_free(target);
            }
        }
    }
    if (g_file_test(temporary, G_FILE_TEST_EXISTS)) {
        g_unlink(temporary);
    }
    g_free(name);
    g_free(destination_dir);
    g_free(temporary);
    return ok;
}

static gboolean extract_archive_to_stage(ExtractJob *job, const gchar *archive,
                                         const gchar *stage, GError **error)
{
    gchar *ext = flitz_archive_extension(archive);
    gboolean ok = FALSE;

    if (!flitz_ensure_directory(stage, error)) {
        g_free(ext);
        return FALSE;
    }

    if (g_ascii_strcasecmp(ext, "zip") == 0) {
        ok = extract_zip(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "tar") == 0 ||
               g_ascii_strcasecmp(ext, "tar.gz") == 0 ||
               g_ascii_strcasecmp(ext, "tar.bz2") == 0 ||
               g_ascii_strcasecmp(ext, "tar.xz") == 0 ||
               g_ascii_strcasecmp(ext, "tgz") == 0 ||
               g_ascii_strcasecmp(ext, "tbz2") == 0 ||
               g_ascii_strcasecmp(ext, "txz") == 0) {
        ok = extract_tar(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "pet") == 0) {
        ok = extract_pet(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "gz") == 0) {
        ok = extract_single_compressed(job, archive, stage, "gzip", error);
    } else if (g_ascii_strcasecmp(ext, "bz2") == 0) {
        ok = extract_single_compressed(job, archive, stage, "bzip2", error);
    } else if (g_ascii_strcasecmp(ext, "xz") == 0) {
        ok = extract_single_compressed(job, archive, stage, "xz", error);
    } else if (g_ascii_strcasecmp(ext, "7z") == 0 ||
               g_ascii_strcasecmp(ext, "tar.7z") == 0 ||
               g_ascii_strcasecmp(ext, "tar.zip") == 0 ||
               g_ascii_strcasecmp(ext, "iso") == 0) {
        ok = extract_7z_generic(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "rar") == 0 ||
               g_ascii_strcasecmp(ext, "001") == 0 ||
               g_ascii_strcasecmp(ext, "r00") == 0) {
        ok = extract_rar(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "deb") == 0) {
        ok = extract_deb(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "rpm") == 0) {
        ok = extract_rpm(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "dmg") == 0) {
        ok = extract_dmg(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "exe") == 0) {
        ok = extract_exe(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "msi") == 0) {
        ok = extract_msi(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "cab") == 0) {
        ok = extract_cab(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "appimage") == 0) {
        ok = extract_appimage(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "sfs") == 0) {
        ok = extract_squashfs(job, archive, stage, error);
    } else if (g_ascii_strcasecmp(ext, "zst") == 0 ||
               g_ascii_strcasecmp(ext, "tar.zst") == 0) {
        ok = extract_zstd(job, archive, stage, error);
    } else {
        flitz_queue_log(&job->app->ui,
                        _("Unknown extension; trying the universal 7z handler."));
        ok = extract_7z_generic(job, archive, stage, error);
    }
    g_free(ext);
    return ok;
}

static void collect_archives_recursive(const gchar *directory,
                                       GPtrArray *paths)
{
    GDir *dir = g_dir_open(directory, 0, NULL);
    const gchar *entry;
    if (dir == NULL) {
        return;
    }
    while ((entry = g_dir_read_name(dir)) != NULL) {
        gchar *path = g_build_filename(directory, entry, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR) &&
            !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
            collect_archives_recursive(path, paths);
        } else if (g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
                   flitz_is_supported_archive(path)) {
            g_ptr_array_add(paths, path);
            path = NULL;
        }
        g_free(path);
    }
    g_dir_close(dir);
}

static gboolean extract_nested_archives(ExtractJob *job,
                                        const gchar *stage,
                                        guint max_depth,
                                        GError **error)
{
    GHashTable *processed = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    guint depth;
    gboolean success = TRUE;

    for (depth = 0; depth < max_depth; depth++) {
        GPtrArray *archives = g_ptr_array_new_with_free_func(g_free);
        guint i;
        guint newly_processed = 0;
        collect_archives_recursive(stage, archives);

        for (i = 0; i < archives->len; i++) {
            const gchar *archive = g_ptr_array_index(archives, i);
            gchar *parent;
            gchar *base;
            gchar *folder_name;
            gchar *destination;
            gchar *message;

            if (g_hash_table_contains(processed, archive)) {
                continue;
            }
            g_hash_table_add(processed, g_strdup(archive));
            newly_processed++;

            parent = g_path_get_dirname(archive);
            base = flitz_archive_basename(archive);
            folder_name = g_strconcat(base, "_extracted", NULL);
            destination = g_build_filename(parent, folder_name, NULL);
            message = g_strdup_printf(_("Recursively extracting: %s"),
                                      archive);
            flitz_queue_log(&job->app->ui, message);
            g_free(message);

            if (g_file_test(destination, G_FILE_TEST_EXISTS)) {
                GError *remove_error = NULL;
                if (!flitz_remove_tree(destination, &remove_error)) {
                    g_propagate_error(error, remove_error);
                    success = FALSE;
                }
            }
            if (success && !extract_archive_to_stage(job, archive, destination,
                                                     error)) {
                success = FALSE;
            }
            g_free(parent);
            g_free(base);
            g_free(folder_name);
            g_free(destination);
            if (!success) {
                break;
            }
        }
        g_ptr_array_free(archives, TRUE);
        if (!success || newly_processed == 0) {
            break;
        }
    }

    g_hash_table_destroy(processed);
    return success;
}

static void commit_line(const gchar *line, gpointer user_data)
{
    ExtractorApp *app = user_data;
    flitz_queue_log(&app->ui, line);
    flitz_queue_progress(&app->ui, 0.0, _("Writing files..."), TRUE);
}

static gboolean extract_finished_idle(gpointer data)
{
    ExtractFinish *finish = data;
    ExtractorApp *app = finish->app;
    app->running = FALSE;
    gtk_widget_set_sensitive(app->extract_button, TRUE);
    if (finish->success) {
        flitz_set_progress(&app->ui, 1.0, _("Extraction completed"));
        flitz_log(&app->ui, _("Extraction completed successfully!"));
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_INFO,
                     _("Success"), finish->message);
    } else {
        flitz_set_progress(&app->ui, 0.0, _("Extraction failed"));
        flitz_logf(&app->ui, _("Error: %s"), finish->message);
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                     _("Extraction failed"), finish->message);
    }
    g_free(finish->message);
    g_free(finish);
    return G_SOURCE_REMOVE;
}

static gpointer extract_worker(gpointer data)
{
    ExtractJob *job = data;
    ExtractFinish *finish = g_new0(ExtractFinish, 1);
    gchar *stage = NULL;
    GError *error = NULL;
    gboolean ok = FALSE;
    gchar *ext;
    gchar *detected;

    finish->app = job->app;
    ext = flitz_archive_extension(job->file_path);
    detected = g_strdup_printf(_("Detected format: %s"), ext);
    flitz_queue_log(&job->app->ui, detected);
    g_free(detected);
    g_free(ext);

    stage = g_dir_make_tmp("flitz-extract-XXXXXX", &error);
    if (stage != NULL) {
        ok = extract_archive_to_stage(job, job->file_path, stage, &error);
    }
    if (ok && job->recursive) {
        flitz_queue_log(&job->app->ui,
                        _("Searching for archives inside the extracted data..."));
        ok = extract_nested_archives(job, stage, 3, &error);
    }
    if (ok) {
        ok = flitz_commit_stage(stage, job->output_dir, job->keep_structure,
                                job->overwrite, commit_line, job->app, &error);
    }

    finish->success = ok;
    if (ok) {
        finish->message = g_strdup_printf(_("Files were extracted to:\n%s"),
                                          job->output_dir);
    } else {
        finish->message = g_strdup(error != NULL ? error->message :
                                   _("Unknown extraction error."));
    }
    g_clear_error(&error);

    if (stage != NULL) {
        GError *cleanup_error = NULL;
        if (!flitz_remove_tree(stage, &cleanup_error)) {
            gchar *warning = g_strdup_printf(_("Warning: temporary directory could not be removed: %s"),
                                             cleanup_error->message);
            flitz_queue_log(&job->app->ui, warning);
            g_free(warning);
        }
        g_clear_error(&cleanup_error);
    }
    g_free(stage);
    g_free(job->file_path);
    g_free(job->output_dir);
    g_free(job);
    g_idle_add(extract_finished_idle, finish);
    return NULL;
}

static void extractor_start(ExtractorApp *app)
{
    ExtractJob *job;
    if (app->running) {
        return;
    }
    if (app->file_path == NULL ||
        !g_file_test(app->file_path, G_FILE_TEST_IS_REGULAR)) {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_ERROR,
                     _("Error"), _("Please select a valid archive first."));
        return;
    }
    if (app->output_dir == NULL || *app->output_dir == '\0') {
        if (!extractor_choose_output(app)) {
            gchar *default_output = flitz_path_dirname(app->file_path);
            extractor_set_output(app, default_output);
            g_free(default_output);
        }
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

    app->running = TRUE;
    gtk_widget_set_sensitive(app->extract_button, FALSE);
    flitz_clear_console(&app->ui);
    flitz_set_progress(&app->ui, 0.0, _("Starting extraction..."));
    flitz_logf(&app->ui, _("File: %s"), app->file_path);
    flitz_logf(&app->ui, _("Output: %s"), app->output_dir);

    job = g_new0(ExtractJob, 1);
    job->app = app;
    job->file_path = g_strdup(app->file_path);
    job->output_dir = g_strdup(app->output_dir);
    job->recursive = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(app->recursive_check));
    job->keep_structure = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(app->keep_check));
    job->overwrite = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(app->overwrite_check));
    {
        GThread *thread = g_thread_new("flitz-extract", extract_worker, job);
        g_thread_unref(thread);
    }
}

static void on_extract_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    extractor_start(user_data);
}

static void extractor_set_output(ExtractorApp *app, const gchar *path)
{
    gchar *short_path;
    g_free(app->output_dir);
    app->output_dir = g_strdup(path);
    short_path = flitz_short_path(path, 42);
    gtk_label_set_text(GTK_LABEL(app->output_label), short_path);
    g_free(short_path);
}

static void extractor_set_file(ExtractorApp *app, const gchar *path)
{
    gchar *base;
    gchar *short_name;
    if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        return;
    }
    g_free(app->file_path);
    app->file_path = g_canonicalize_filename(path, NULL);
    base = g_path_get_basename(app->file_path);
    short_name = flitz_short_path(base, 35);
    gtk_label_set_text(GTK_LABEL(app->file_label), short_name);
    gtk_widget_set_sensitive(app->extract_button, TRUE);
    flitz_logf(&app->ui, _("File: %s"), base);
    g_free(short_name);
    g_free(base);
}

static void on_select_file(GtkButton *button, gpointer user_data)
{
    ExtractorApp *app = user_data;
    GtkWidget *dialog;
    GtkFileFilter *filter;
    (void)button;
    dialog = gtk_file_chooser_dialog_new(_("Select archive"),
                                         GTK_WINDOW(app->ui.window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         _("Cancel"), GTK_RESPONSE_CANCEL,
                                         _("Select"), GTK_RESPONSE_ACCEPT,
                                         NULL);
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("All supported archives"));
    gtk_file_filter_add_pattern(filter, "*.zip");
    gtk_file_filter_add_pattern(filter, "*.tar");
    gtk_file_filter_add_pattern(filter, "*.tar.*");
    gtk_file_filter_add_pattern(filter, "*.tgz");
    gtk_file_filter_add_pattern(filter, "*.gz");
    gtk_file_filter_add_pattern(filter, "*.bz2");
    gtk_file_filter_add_pattern(filter, "*.xz");
    gtk_file_filter_add_pattern(filter, "*.7z");
    gtk_file_filter_add_pattern(filter, "*.rar");
    gtk_file_filter_add_pattern(filter, "*.pet");
    gtk_file_filter_add_pattern(filter, "*.deb");
    gtk_file_filter_add_pattern(filter, "*.rpm");
    gtk_file_filter_add_pattern(filter, "*.dmg");
    gtk_file_filter_add_pattern(filter, "*.iso");
    gtk_file_filter_add_pattern(filter, "*.exe");
    gtk_file_filter_add_pattern(filter, "*.msi");
    gtk_file_filter_add_pattern(filter, "*.cab");
    gtk_file_filter_add_pattern(filter, "*.AppImage");
    gtk_file_filter_add_pattern(filter, "*.appimage");
    gtk_file_filter_add_pattern(filter, "*.sfs");
    gtk_file_filter_add_pattern(filter, "*.squashfs");
    gtk_file_filter_add_pattern(filter, "*.zst");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        extractor_set_file(app, filename);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

static gboolean extractor_choose_output(ExtractorApp *app)
{
    GtkWidget *dialog;
    gboolean selected = FALSE;
    dialog = gtk_file_chooser_dialog_new(_("Select output directory"),
                                         GTK_WINDOW(app->ui.window),
                                         GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                         _("Cancel"), GTK_RESPONSE_CANCEL,
                                         _("Select"), GTK_RESPONSE_ACCEPT,
                                         NULL);
    if (app->output_dir != NULL) {
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog),
                                      app->output_dir);
    } else if (app->file_path != NULL) {
        gchar *parent = flitz_path_dirname(app->file_path);
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), parent);
        g_free(parent);
    }
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        gchar *directory = gtk_file_chooser_get_filename(
            GTK_FILE_CHOOSER(dialog));
        extractor_set_output(app, directory);
        flitz_logf(&app->ui, _("Output: %s"), directory);
        g_free(directory);
        selected = TRUE;
    }
    gtk_widget_destroy(dialog);
    return selected;
}

static void on_select_output(GtkButton *button, gpointer user_data)
{
    (void)button;
    extractor_choose_output(user_data);
}

static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context,
                                  gint x, gint y,
                                  GtkSelectionData *selection_data,
                                  guint info, guint time,
                                  gpointer user_data)
{
    ExtractorApp *app = user_data;
    gchar *path = flitz_first_dropped_path(selection_data);
    gboolean accepted = FALSE;
    (void)widget;
    (void)x;
    (void)y;
    (void)info;
    if (path != NULL && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        extractor_set_file(app, path);
        accepted = TRUE;
    }
    gtk_drag_finish(context, accepted, FALSE, time);
    g_free(path);
}

static gboolean on_window_delete(GtkWidget *widget, GdkEvent *event,
                                 gpointer user_data)
{
    ExtractorApp *app = user_data;
    (void)widget;
    (void)event;
    if (app->running) {
        show_message(GTK_WINDOW(app->ui.window), GTK_MESSAGE_WARNING,
                     _("Operation in progress"),
                     _("Wait for extraction to finish before closing Flitz."));
        return TRUE;
    }
    return FALSE;
}

static void on_about_clicked(GtkButton *button, gpointer user_data)
{
    ExtractorApp *app = user_data;
    (void)button;
    flitz_show_about(GTK_WINDOW(app->ui.window),
                     _("Flitz Universal Extractor"));
}

static GtkWidget *make_labeled_frame(const gchar *title)
{
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(frame), 2);
    return frame;
}

static GtkWidget *build_ui(ExtractorApp *app)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *image;
    GtkWidget *title;
    GtkWidget *byline;
    GtkWidget *about_button;
    GtkWidget *file_frame;
    GtkWidget *file_box;
    GtkWidget *file_button;
    GtkWidget *output_frame;
    GtkWidget *output_box;
    GtkWidget *output_button;
    GtkWidget *options_frame;
    GtkWidget *options_box;
    GtkWidget *console_frame;
    GtkWidget *scrolled;
    GtkWidget *console;
    GtkWidget *progress;
    GtkWidget *progress_label;
    GtkWidget *extract_button;
    GtkTargetEntry targets[] = { { "text/uri-list", 0, 0 } };

    gtk_window_set_title(GTK_WINDOW(window), _("Flitz Universal Extractor"));
    gtk_window_set_default_size(GTK_WINDOW(window), 450, 520);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(window), 6);
    flitz_set_window_icon(GTK_WINDOW(window));

    gtk_container_add(GTK_CONTAINER(window), main_box);
    image = flitz_create_logo(48);
    gtk_box_pack_start(GTK_BOX(top_box), image, FALSE, FALSE, 3);
    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
                         _("<b>Flitz Universal Extractor</b>"));
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

    file_frame = make_labeled_frame(_("File"));
    file_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(file_box), 4);
    gtk_container_add(GTK_CONTAINER(file_frame), file_box);
    file_button = gtk_button_new_with_label(_("Select"));
    app->file_label = gtk_label_new(_("Drag file here or click Select"));
    gtk_label_set_ellipsize(GTK_LABEL(app->file_label), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(app->file_label, TRUE);
    gtk_widget_set_halign(app->file_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(file_box), file_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(file_box), app->file_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), file_frame, FALSE, FALSE, 0);

    output_frame = make_labeled_frame(_("Output"));
    output_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(output_box), 4);
    gtk_container_add(GTK_CONTAINER(output_frame), output_box);
    output_button = gtk_button_new_with_label(_("Select"));
    app->output_label = gtk_label_new(_("Current"));
    gtk_label_set_ellipsize(GTK_LABEL(app->output_label), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(app->output_label, TRUE);
    gtk_widget_set_halign(app->output_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(output_box), output_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(output_box), app->output_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), output_frame, FALSE, FALSE, 0);

    options_frame = make_labeled_frame(_("Options"));
    options_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(options_box), 4);
    gtk_container_add(GTK_CONTAINER(options_frame), options_box);
    app->recursive_check = gtk_check_button_new_with_label(_("Recursive"));
    app->keep_check = gtk_check_button_new_with_label(_("Keep structure"));
    app->overwrite_check = gtk_check_button_new_with_label(_("Overwrite"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->keep_check), TRUE);
    gtk_box_pack_start(GTK_BOX(options_box), app->recursive_check,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(options_box), app->keep_check,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(options_box), app->overwrite_check,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), options_frame, FALSE, FALSE, 0);

    progress = gtk_progress_bar_new();
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(progress), 0.05);
    progress_label = gtk_label_new(_("Ready"));
    gtk_box_pack_start(GTK_BOX(main_box), progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), progress_label, FALSE, FALSE, 0);

    console_frame = make_labeled_frame(_("Console"));
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

    extract_button = gtk_button_new_with_label(_("Extract"));
    gtk_widget_set_sensitive(extract_button, FALSE);
    gtk_box_pack_start(GTK_BOX(main_box), extract_button, FALSE, FALSE, 2);
    app->extract_button = extract_button;

    flitz_ui_init(&app->ui, window, console, progress, progress_label);

    gtk_drag_dest_set(window, GTK_DEST_DEFAULT_ALL, targets, 1,
                      GDK_ACTION_COPY);
    gtk_drag_dest_set(app->file_label, GTK_DEST_DEFAULT_ALL, targets, 1,
                      GDK_ACTION_COPY);
    g_signal_connect(window, "drag-data-received",
                     G_CALLBACK(on_drag_data_received), app);
    g_signal_connect(app->file_label, "drag-data-received",
                     G_CALLBACK(on_drag_data_received), app);
    g_signal_connect(file_button, "clicked", G_CALLBACK(on_select_file), app);
    g_signal_connect(about_button, "clicked", G_CALLBACK(on_about_clicked), app);
    g_signal_connect(output_button, "clicked", G_CALLBACK(on_select_output),
                     app);
    g_signal_connect(extract_button, "clicked",
                     G_CALLBACK(on_extract_clicked), app);
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

static void parse_args(ExtractorApp *app, int argc, char **argv,
                       gchar **initial_file, gchar **initial_output,
                       gboolean *recursive, gboolean *keep_structure,
                       gboolean *overwrite)
{
    int i;
    *keep_structure = TRUE;
    for (i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "-o") == 0 ||
            g_strcmp0(argv[i], "--output") == 0) {
            if (i + 1 < argc) {
                g_free(*initial_output);
                *initial_output = g_strdup(argv[++i]);
            }
        } else if (g_str_has_prefix(argv[i], "--output=")) {
            g_free(*initial_output);
            *initial_output = g_strdup(argv[i] + strlen("--output="));
        } else if (g_strcmp0(argv[i], "-r") == 0 ||
                   g_strcmp0(argv[i], "--recursive") == 0) {
            *recursive = TRUE;
        } else if (g_strcmp0(argv[i], "-k") == 0 ||
                   g_strcmp0(argv[i], "--keep-structure") == 0) {
            *keep_structure = TRUE;
        } else if (g_strcmp0(argv[i], "--no-keep-structure") == 0) {
            *keep_structure = FALSE;
        } else if (g_strcmp0(argv[i], "-y") == 0 ||
                   g_strcmp0(argv[i], "--overwrite") == 0) {
            *overwrite = TRUE;
        } else if (g_strcmp0(argv[i], "-l") == 0 ||
                   g_strcmp0(argv[i], "--language") == 0) {
            i++;
        } else if (g_str_has_prefix(argv[i], "--language=")) {
            continue;
        } else if (argv[i][0] != '-') {
            g_free(*initial_file);
            *initial_file = g_strdup(argv[i]);
            app->auto_start = TRUE;
        }
    }
}

static void log_missing_tools(ExtractorApp *app)
{
    static const struct {
        const gchar *command;
        const gchar *package;
    } tools[] = {
        { "7z", "p7zip" },
        { "unrar", "unrar" },
        { "cabextract", "cabextract" },
        { "rpm2cpio", "rpm-tools" },
        { "cpio", "cpio" },
        { "dmg2img", "dmg2img" },
        { "innoextract", "innoextract" },
        { "msiextract", "msitools" },
        { "ar", "binutils" },
        { "unsquashfs", "squashfs-tools" },
        { "zstd", "zstd" }
    };
    GString *missing = g_string_new(NULL);
    guint i;
    for (i = 0; i < G_N_ELEMENTS(tools); i++) {
        if (!flitz_command_exists(tools[i].command)) {
            if (missing->len > 0) {
                g_string_append_c(missing, ' ');
            }
            g_string_append(missing, tools[i].package);
        }
    }
    if (missing->len > 0) {
        flitz_logf(&app->ui, _("Warning: missing optional tools: %s"),
                   missing->str);
    }
    g_string_free(missing, TRUE);
}

static gboolean auto_start_idle(gpointer data)
{
    extractor_start(data);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    ExtractorApp *app;
    gchar *initial_file = NULL;
    gchar *initial_output = NULL;
    gboolean recursive = FALSE;
    gboolean keep_structure = TRUE;
    gboolean overwrite = FALSE;
    const gchar *language = scan_language_arg(argc, argv);

    flitz_init_i18n(language);
    gtk_init(&argc, &argv);

    app = g_new0(ExtractorApp, 1);
    parse_args(app, argc, argv, &initial_file, &initial_output, &recursive,
               &keep_structure, &overwrite);
    app->ui.window = build_ui(app);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->recursive_check),
                                 recursive);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->keep_check),
                                 keep_structure);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->overwrite_check),
                                 overwrite);
    if (initial_file != NULL &&
        g_file_test(initial_file, G_FILE_TEST_IS_REGULAR)) {
        extractor_set_file(app, initial_file);
        if (app->auto_start && initial_output == NULL) {
            gchar *parent = flitz_path_dirname(initial_file);
            extractor_set_output(app, parent);
            g_free(parent);
        }
    }
    if (initial_output != NULL) {
        extractor_set_output(app, initial_output);
    }

    gtk_widget_show_all(app->ui.window);
    log_missing_tools(app);
    if (app->auto_start && app->file_path != NULL) {
        g_idle_add(auto_start_idle, app);
    }
    gtk_main();

    g_free(initial_file);
    g_free(initial_output);
    g_free(app->file_path);
    g_free(app->output_dir);
    g_free(app);
    return 0;
}
