#include "app.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * ForgeFM 0.2 MVP
 *
 * Deliberately built around GIO so filesystem behavior stays in the GLib
 * abstraction instead of being tied to POSIX-only calls.
 */

typedef struct {
    GFile *file;
    gboolean move;
} ClipboardItem;

struct _ForgeApp {
    GtkApplication *gtk_app;
    GtkWidget *window;
    GtkWidget *path_entry;
    GtkWidget *search_entry;
    GtkWidget *file_list;
    GtkWidget *status_label;
    GtkWidget *hidden_toggle;
    GtkWidget *paste_button;

    GFile *current_dir;
    GPtrArray *history;
    gint history_index;

    GPtrArray *clipboard; /* ClipboardItem* */
    gboolean clipboard_move;
    gchar *search_text;
};

static void navigate_to(ForgeApp *app, GFile *dir, gboolean add_history);
static void refresh_directory(ForgeApp *app);
static void show_message(ForgeApp *app, const char *title, const char *message);

static void clipboard_item_free(ClipboardItem *item)
{
    if (!item)
        return;
    g_clear_object(&item->file);
    g_free(item);
}

static gchar *format_size(goffset size)
{
    if (size < 1024)
        return g_strdup_printf("%" G_GOFFSET_FORMAT " B", size);
    if (size < 1024LL * 1024LL)
        return g_strdup_printf("%.1f KB", (double)size / 1024.0);
    if (size < 1024LL * 1024LL * 1024LL)
        return g_strdup_printf("%.1f MB", (double)size / (1024.0 * 1024.0));
    if (size < 1024LL * 1024LL * 1024LL * 1024LL)
        return g_strdup_printf("%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
    return g_strdup_printf("%.1f TB", (double)size / (1024.0 * 1024.0 * 1024.0 * 1024.0));
}

static gchar *format_time(GFileInfo *info)
{
    GDateTime *dt = g_file_info_get_modification_date_time(info);
    if (!dt)
        return g_strdup("-");
    gchar *text = g_date_time_format(dt, "%Y-%m-%d %H:%M");
    g_date_time_unref(dt);
    return text;
}

static const char *file_kind(GFileInfo *info)
{
    switch (g_file_info_get_file_type(info)) {
    case G_FILE_TYPE_DIRECTORY: return "Folder";
    case G_FILE_TYPE_REGULAR: return "File";
    case G_FILE_TYPE_SYMBOLIC_LINK: return "Symlink";
    case G_FILE_TYPE_SPECIAL: return "Special";
    case G_FILE_TYPE_SHORTCUT: return "Shortcut";
    case G_FILE_TYPE_MOUNTABLE: return "Mountable";
    default: return "Unknown";
    }
}

static const char *icon_for_info(GFileInfo *info)
{
    switch (g_file_info_get_file_type(info)) {
    case G_FILE_TYPE_DIRECTORY:
        return "folder-symbolic";
    case G_FILE_TYPE_SYMBOLIC_LINK:
        return "insert-link-symbolic";
    default:
        return "text-x-generic-symbolic";
    }
}

static void set_status(ForgeApp *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status_label), text ? text : "");
}

static void show_error(ForgeApp *app, const char *prefix, GError *error)
{
    gchar *text = g_strdup_printf("%s: %s", prefix, error ? error->message : "Unknown error");
    set_status(app, text);
    show_message(app, "ForgeFM", text);
    g_free(text);
}

static void show_message(ForgeApp *app, const char *title, const char *message)
{
    GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
    const char *buttons[] = {"OK", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_message(dialog, message);
    gtk_alert_dialog_show(dialog, GTK_WINDOW(app->window));
    g_object_unref(dialog);
}

static void clear_list(ForgeApp *app)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->file_list)) != NULL)
        gtk_list_box_remove(GTK_LIST_BOX(app->file_list), child);
}

static gboolean name_matches_search(ForgeApp *app, GFileInfo *info)
{
    if (!app->search_text || !*app->search_text)
        return TRUE;

    gchar *name = g_utf8_casefold(g_file_info_get_display_name(info), -1);
    gchar *needle = g_utf8_casefold(app->search_text, -1);
    gboolean matches = strstr(name, needle) != NULL;
    g_free(name);
    g_free(needle);
    return matches;
}

static int compare_infos(gconstpointer a, gconstpointer b)
{
    const GFileInfo *ia = *(GFileInfo * const *)a;
    const GFileInfo *ib = *(GFileInfo * const *)b;

    gboolean da = g_file_info_get_file_type((GFileInfo *)ia) == G_FILE_TYPE_DIRECTORY;
    gboolean db = g_file_info_get_file_type((GFileInfo *)ib) == G_FILE_TYPE_DIRECTORY;
    if (da != db)
        return db - da;

    return g_utf8_collate(
        g_file_info_get_display_name((GFileInfo *)ia),
        g_file_info_get_display_name((GFileInfo *)ib)
    );
}

static GtkWidget *make_file_row(ForgeApp *app, GFile *file, GFileInfo *info)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(box, 7);
    gtk_widget_set_margin_bottom(box, 7);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);

    GtkWidget *icon = gtk_image_new_from_icon_name(icon_for_info(info));
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 26);

    GtkWidget *name_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *name = gtk_label_new(g_file_info_get_display_name(info));
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_widget_add_css_class(name, "heading");

    gchar *size_text = NULL;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
        size_text = g_strdup("Directory");
    else
        size_text = format_size(g_file_info_get_size(info));

    gchar *details = g_strdup_printf("%s  •  %s", file_kind(info), size_text);
    GtkWidget *subtitle = gtk_label_new(details);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(subtitle, "dim-label");
    g_free(details);
    g_free(size_text);

    gtk_box_append(GTK_BOX(name_box), name);
    gtk_box_append(GTK_BOX(name_box), subtitle);
    gtk_widget_set_hexpand(name_box, TRUE);

    gchar *time_text = format_time(info);
    GtkWidget *time = gtk_label_new(time_text);
    gtk_widget_add_css_class(time, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(time), 1.0f);
    g_free(time_text);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), name_box);
    gtk_box_append(GTK_BOX(box), time);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    g_object_set_data_full(G_OBJECT(row), "forge-file", g_object_ref(file), g_object_unref);
    g_object_set_data(G_OBJECT(row), "forge-is-dir",
                      GINT_TO_POINTER(g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY));
    return row;
}

static void update_location(ForgeApp *app)
{
    gchar *path = g_file_get_path(app->current_dir);
    if (path) {
        gtk_editable_set_text(GTK_EDITABLE(app->path_entry), path);
        g_free(path);
        return;
    }

    gchar *uri = g_file_get_uri(app->current_dir);
    gtk_editable_set_text(GTK_EDITABLE(app->path_entry), uri ? uri : "");
    g_free(uri);
}

static void refresh_directory(ForgeApp *app)
{
    clear_list(app);
    if (!app->current_dir)
        return;

    gboolean show_hidden = gtk_switch_get_active(GTK_SWITCH(app->hidden_toggle));
    GError *error = NULL;
    GFileEnumerator *enumerator = g_file_enumerate_children(
        app->current_dir,
        "standard::name,standard::display-name,standard::type,standard::size,time::modified,standard::is-hidden",
        G_FILE_QUERY_INFO_NONE,
        NULL,
        &error
    );

    if (!enumerator) {
        show_error(app, "Cannot read directory", error);
        g_clear_error(&error);
        return;
    }

    GPtrArray *infos = g_ptr_array_new_with_free_func(g_object_unref);
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(enumerator, NULL, &error)) != NULL) {
        gboolean hidden = g_file_info_get_is_hidden(info) ||
                          g_str_has_prefix(g_file_info_get_name(info), ".");
        if ((!show_hidden && hidden) || !name_matches_search(app, info)) {
            g_object_unref(info);
            continue;
        }
        g_ptr_array_add(infos, info);
    }

    if (error) {
        show_error(app, "Directory enumeration failed", error);
        g_clear_error(&error);
    }

    g_ptr_array_sort(infos, compare_infos);

    for (guint i = 0; i < infos->len; ++i) {
        GFileInfo *item = g_ptr_array_index(infos, i);
        GFile *file = g_file_get_child(app->current_dir, g_file_info_get_name(item));
        GtkWidget *row = make_file_row(app, file, item);
        gtk_list_box_append(GTK_LIST_BOX(app->file_list), row);
        g_object_unref(file);
    }

    gchar *status = g_strdup_printf("%u item%s", infos->len, infos->len == 1 ? "" : "s");
    if (app->search_text && *app->search_text) {
        gchar *with_search = g_strdup_printf("%s matching \"%s\"", status, app->search_text);
        set_status(app, with_search);
        g_free(with_search);
    } else {
        set_status(app, status);
    }
    g_free(status);

    g_ptr_array_unref(infos);
    g_object_unref(enumerator);
}

static void navigate_to(ForgeApp *app, GFile *dir, gboolean add_history)
{
    if (!dir)
        return;

    if (add_history) {
        while ((gint)app->history->len - 1 > app->history_index)
            g_ptr_array_remove_index(app->history, app->history->len - 1);

        g_ptr_array_add(app->history, g_object_ref(dir));
        app->history_index = (gint)app->history->len - 1;
    }

    g_set_object(&app->current_dir, dir);
    update_location(app);
    refresh_directory(app);
}

static void go_back(ForgeApp *app)
{
    if (app->history_index <= 0)
        return;
    app->history_index--;
    navigate_to(app, g_ptr_array_index(app->history, app->history_index), FALSE);
}

static void go_forward(ForgeApp *app)
{
    if (app->history_index + 1 >= (gint)app->history->len)
        return;
    app->history_index++;
    navigate_to(app, g_ptr_array_index(app->history, app->history_index), FALSE);
}

static void go_up(ForgeApp *app)
{
    if (!app->current_dir)
        return;
    GFile *parent = g_file_get_parent(app->current_dir);
    if (!parent)
        return;
    navigate_to(app, parent, TRUE);
    g_object_unref(parent);
}

static void open_file_or_directory(ForgeApp *app, GFile *file)
{
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (!info) {
        show_error(app, "Cannot open item", error);
        g_clear_error(&error);
        return;
    }

    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        navigate_to(app, file, TRUE);
    } else {
        gchar *uri = g_file_get_uri(file);
        if (!g_app_info_launch_default_for_uri(uri, NULL, &error)) {
            show_error(app, "Cannot open file", error);
            g_clear_error(&error);
        } else {
            set_status(app, "Opened file");
        }
        g_free(uri);
    }

    g_object_unref(info);
}

static GPtrArray *selected_files(ForgeApp *app)
{
    GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
    GList *rows = gtk_list_box_get_selected_rows(GTK_LIST_BOX(app->file_list));
    for (GList *it = rows; it; it = it->next) {
        GtkListBoxRow *row = it->data;
        GFile *file = g_object_get_data(G_OBJECT(row), "forge-file");
        if (file)
            g_ptr_array_add(files, g_object_ref(file));
    }
    g_list_free(rows);
    return files;
}

static void clear_clipboard(ForgeApp *app)
{
    g_ptr_array_set_size(app->clipboard, 0);
    app->clipboard_move = FALSE;
    gtk_widget_set_sensitive(app->paste_button, FALSE);
}

static void copy_selection(ForgeApp *app, gboolean move)
{
    GPtrArray *files = selected_files(app);
    if (files->len == 0) {
        g_ptr_array_unref(files);
        return;
    }

    g_ptr_array_set_size(app->clipboard, 0);
    for (guint i = 0; i < files->len; ++i) {
        GFile *file = g_ptr_array_index(files, i);
        ClipboardItem *item = g_new0(ClipboardItem, 1);
        item->file = g_object_ref(file);
        item->move = move;
        g_ptr_array_add(app->clipboard, item);
    }
    app->clipboard_move = move;
    gtk_widget_set_sensitive(app->paste_button, TRUE);

    gchar *msg = g_strdup_printf("%u item%s %s",
                                 files->len,
                                 files->len == 1 ? "" : "s",
                                 move ? "cut" : "copied");
    set_status(app, msg);
    g_free(msg);
    g_ptr_array_unref(files);
}

static gchar *unique_destination_name(GFile *directory, const char *basename)
{
    GFile *candidate = g_file_get_child(directory, basename);
    if (!g_file_query_exists(candidate, NULL)) {
        g_object_unref(candidate);
        return g_strdup(basename);
    }
    g_object_unref(candidate);

    gchar *name = g_strdup(basename);
    const char *dot = strrchr(basename, '.');
    gchar *stem = NULL;
    gchar *ext = NULL;
    if (dot && dot != basename) {
        stem = g_strndup(basename, dot - basename);
        ext = g_strdup(dot);
    } else {
        stem = g_strdup(basename);
        ext = g_strdup("");
    }
    g_free(name);

    for (guint n = 1; n < 10000; ++n) {
        name = g_strdup_printf("%s (%u)%s", stem, n, ext);
        candidate = g_file_get_child(directory, name);
        gboolean exists = g_file_query_exists(candidate, NULL);
        g_object_unref(candidate);
        if (!exists)
            break;
        g_free(name);
        name = NULL;
    }

    g_free(stem);
    g_free(ext);
    return name ? name : g_strdup(basename);
}

static void paste_clipboard(ForgeApp *app)
{
    if (!app->current_dir || app->clipboard->len == 0)
        return;

    guint done = 0;
    GPtrArray *done_items = g_ptr_array_new();

    for (guint i = 0; i < app->clipboard->len; ++i) {
        ClipboardItem *item = g_ptr_array_index(app->clipboard, i);
        gchar *basename = g_file_get_basename(item->file);
        gchar *dest_name = unique_destination_name(app->current_dir, basename);
        GFile *dest = g_file_get_child(app->current_dir, dest_name);
        GError *error = NULL;
        gboolean ok;

        if (item->move) {
            ok = g_file_move(item->file, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
        } else {
            ok = g_file_copy(item->file, dest, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
        }

        if (ok) {
            done++;
            g_ptr_array_add(done_items, item);
        } else if (error) {
            show_error(app, "Paste failed", error);
            g_clear_error(&error);
        }

        g_object_unref(dest);
        g_free(dest_name);
        g_free(basename);
    }

    if (app->clipboard_move)
        clear_clipboard(app);

    refresh_directory(app);

    gchar *msg = g_strdup_printf("%u item%s pasted", done, done == 1 ? "" : "s");
    set_status(app, msg);
    g_free(msg);
    g_ptr_array_unref(done_items);
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)box;
    ForgeApp *app = user_data;
    GFile *file = g_object_get_data(G_OBJECT(row), "forge-file");
    if (file)
        open_file_or_directory(app, file);
}

static void on_reload(GtkButton *button, gpointer user_data)
{
    (void)button;
    refresh_directory(user_data);
}

static void on_back(GtkButton *button, gpointer user_data)
{
    (void)button;
    go_back(user_data);
}

static void on_forward(GtkButton *button, gpointer user_data)
{
    (void)button;
    go_forward(user_data);
}

static void on_up(GtkButton *button, gpointer user_data)
{
    (void)button;
    go_up(user_data);
}

static void on_hidden_toggled(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    refresh_directory(user_data);
}

static void on_search_changed(GtkEditable *editable, gpointer user_data)
{
    ForgeApp *app = user_data;
    g_free(app->search_text);
    app->search_text = g_strdup(gtk_editable_get_text(editable));
    refresh_directory(app);
}

static void on_location_activate(GtkEntry *entry, gpointer user_data)
{
    ForgeApp *app = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!text || !*text)
        return;

    GFile *file = g_file_new_for_commandline_arg(text);
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                        G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (!info) {
        show_error(app, "Invalid location", error);
        g_clear_error(&error);
        g_object_unref(file);
        return;
    }

    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
        navigate_to(app, file, TRUE);
    else
        set_status(app, "That path is not a directory");

    g_object_unref(info);
    g_object_unref(file);
}

static void finish_simple_dialog(GtkDialog *dialog, int response, gpointer user_data)
{
    ForgeApp *app = user_data;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "forge-entry");
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (!name || !*name) {
        gtk_window_destroy(GTK_WINDOW(dialog));
        return;
    }

    gboolean folder = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "forge-folder"));
    GFile *child = g_file_get_child(app->current_dir, name);
    GError *error = NULL;
    gboolean ok;

    if (folder) {
        ok = g_file_make_directory(child, NULL, &error);
    } else {
        GFileOutputStream *stream = g_file_create(child, G_FILE_CREATE_NONE, NULL, &error);
        ok = stream != NULL;
        if (stream)
            g_object_unref(stream);
    }

    if (!ok) {
        show_error(app, folder ? "Could not create folder" : "Could not create file", error);
        g_clear_error(&error);
    } else {
        refresh_directory(app);
    }

    g_object_unref(child);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void create_named(ForgeApp *app, gboolean folder)
{
    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(dialog), folder ? "New Folder" : "New File");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 360, -1);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), folder ? "Folder name" : "File name");
    gtk_box_append(GTK_BOX(content), entry);
    g_object_set_data(G_OBJECT(dialog), "forge-entry", entry);
    g_object_set_data(G_OBJECT(dialog), "forge-folder", GINT_TO_POINTER(folder));

    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), folder ? "Create Folder" : "Create File", GTK_RESPONSE_ACCEPT);
    g_signal_connect(dialog, "response", G_CALLBACK(finish_simple_dialog), app);

    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(entry);
}

static void rename_response(GtkDialog *dialog, int response, gpointer user_data)
{
    ForgeApp *app = user_data;
    GFile *file = g_object_get_data(G_OBJECT(dialog), "forge-file");

    if (response == GTK_RESPONSE_ACCEPT && file) {
        GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "forge-entry");
        const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (name && *name) {
            GError *error = NULL;
            GFile *dest = g_file_get_parent(file);
            if (dest) {
                GFile *renamed = g_file_get_child(dest, name);
                if (!g_file_move(file, renamed, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
                    show_error(app, "Rename failed", error);
                    g_clear_error(&error);
                } else {
                    refresh_directory(app);
                }
                g_object_unref(renamed);
                g_object_unref(dest);
            }
        }
    }

    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void rename_selected(ForgeApp *app)
{
    GPtrArray *files = selected_files(app);
    if (files->len != 1) {
        g_ptr_array_unref(files);
        set_status(app, "Select exactly one item to rename");
        return;
    }

    GFile *file = g_ptr_array_index(files, 0);
    gchar *basename = g_file_get_basename(file);

    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(dialog), "Rename");

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 18);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);

    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), basename);
    gtk_box_append(GTK_BOX(content), entry);
    g_object_set_data(G_OBJECT(dialog), "forge-entry", entry);
    g_object_set_data_full(G_OBJECT(dialog), "forge-file", g_object_ref(file), g_object_unref);

    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Rename", GTK_RESPONSE_ACCEPT);
    g_signal_connect(dialog, "response", G_CALLBACK(rename_response), app);

    gtk_window_present(GTK_WINDOW(dialog));
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);

    g_free(basename);
    g_ptr_array_unref(files);
}

static void delete_response(GtkDialog *dialog, int response, gpointer user_data)
{
    ForgeApp *app = user_data;
    if (response == GTK_RESPONSE_ACCEPT) {
        GPtrArray *files = g_object_steal_data(G_OBJECT(dialog), "forge-files");
        guint done = 0;
        for (guint i = 0; i < files->len; ++i) {
            GFile *file = g_ptr_array_index(files, i);
            GError *error = NULL;
            if (g_file_trash(file, NULL, &error)) {
                done++;
            } else {
                show_error(app, "Trash failed", error);
                g_clear_error(&error);
            }
        }
        refresh_directory(app);
        gchar *msg = g_strdup_printf("Moved %u item%s to trash", done, done == 1 ? "" : "s");
        set_status(app, msg);
        g_free(msg);
        g_ptr_array_unref(files);
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void delete_selected(ForgeApp *app)
{
    GPtrArray *files = selected_files(app);
    if (files->len == 0) {
        g_ptr_array_unref(files);
        return;
    }

    gchar *message = g_strdup_printf("Move %u selected item%s to the trash?",
                                     files->len,
                                     files->len == 1 ? "" : "s");

    GtkWidget *dialog = gtk_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_title(GTK_WINDOW(dialog), "Move to Trash");

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *label = gtk_label_new(message);
    gtk_widget_set_margin_top(label, 18);
    gtk_widget_set_margin_bottom(label, 18);
    gtk_widget_set_margin_start(label, 18);
    gtk_widget_set_margin_end(label, 18);
    gtk_box_append(GTK_BOX(content), label);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dialog), "Move to Trash", GTK_RESPONSE_ACCEPT);

    g_object_set_data_full(G_OBJECT(dialog), "forge-files", files,
                           (GDestroyNotify)g_ptr_array_unref);
    g_signal_connect(dialog, "response", G_CALLBACK(delete_response), app);
    gtk_window_present(GTK_WINDOW(dialog));
    g_free(message);
}

static void open_terminal_here(ForgeApp *app)
{
    gchar *terminal = g_find_program_in_path("xdg-terminal-exec");
    if (!terminal)
        terminal = g_find_program_in_path("x-terminal-emulator");
    if (!terminal) {
        set_status(app, "No terminal launcher found (xdg-terminal-exec / x-terminal-emulator)");
        return;
    }

    gchar *path = g_file_get_path(app->current_dir);
    gchar *argv[] = {terminal, NULL};
    GError *error = NULL;
    if (!g_spawn_async(path, argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, &error)) {
        show_error(app, "Could not open terminal", error);
        g_clear_error(&error);
    } else {
        set_status(app, "Opened terminal here");
    }
    g_free(path);
    g_free(terminal);
}

static void sidebar_clicked(GtkButton *button, gpointer user_data)
{
    ForgeApp *app = user_data;
    const char *path = g_object_get_data(G_OBJECT(button), "forge-path");
    if (!path)
        return;
    GFile *dir = g_file_new_for_path(path);
    navigate_to(app, dir, TRUE);
    g_object_unref(dir);
}

static gboolean key_pressed(GtkEventControllerKey *controller,
                        guint keyval,
                        guint keycode,
                        GdkModifierType state,
                        gpointer user_data)
{
    (void)controller;
    (void)keycode;
    ForgeApp *app = user_data;
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;

    if (ctrl && keyval == GDK_KEY_l) {
        gtk_widget_grab_focus(app->path_entry);
        gtk_editable_select_region(GTK_EDITABLE(app->path_entry), 0, -1);
        return GDK_EVENT_STOP;
    }
    if (ctrl && keyval == GDK_KEY_f) {
        gtk_widget_grab_focus(app->search_entry);
        return GDK_EVENT_STOP;
    }
    if (ctrl && keyval == GDK_KEY_r) {
        refresh_directory(app);
        return GDK_EVENT_STOP;
    }
    if (ctrl && keyval == GDK_KEY_c) {
        copy_selection(app, FALSE);
        return GDK_EVENT_STOP;
    }
    if (ctrl && keyval == GDK_KEY_x) {
        copy_selection(app, TRUE);
        return GDK_EVENT_STOP;
    }
    if (ctrl && keyval == GDK_KEY_v) {
        paste_clipboard(app);
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_F2) {
        rename_selected(app);
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_Delete) {
        delete_selected(app);
        return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_BackSpace) {
        go_up(app);
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static GtkWidget *sidebar_button(const char *label, const char *icon, const char *path, ForgeApp *app)
{
    GtkWidget *button = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *image = gtk_image_new_from_icon_name(icon);
    GtkWidget *text = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), text);
    gtk_button_set_child(GTK_BUTTON(button), box);
    g_object_set_data_full(G_OBJECT(button), "forge-path", g_strdup(path), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(sidebar_clicked), app);
    return button;
}

static GtkWidget *make_sidebar(ForgeApp *app)
{
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(sidebar, 220, -1);
    gtk_widget_set_margin_start(sidebar, 8);
    gtk_widget_set_margin_top(sidebar, 8);
    gtk_widget_set_margin_end(sidebar, 4);
    gtk_widget_set_margin_bottom(sidebar, 8);

    GtkWidget *title = gtk_label_new("Places");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_add_css_class(title, "title-3");
    gtk_widget_set_margin_start(title, 10);
    gtk_widget_set_margin_bottom(title, 8);
    gtk_box_append(GTK_BOX(sidebar), title);

    const char *home = g_get_home_dir();
    gtk_box_append(GTK_BOX(sidebar), sidebar_button("Home", "user-home-symbolic", home, app));

    const char *names[] = {"Desktop", "Documents", "Downloads", "Music", "Pictures", "Videos"};
    const char *icons[] = {
        "user-desktop-symbolic", "folder-documents-symbolic", "folder-download-symbolic",
        "folder-music-symbolic", "folder-pictures-symbolic", "folder-videos-symbolic"
    };

    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *path = g_build_filename(home, names[i], NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR))
            gtk_box_append(GTK_BOX(sidebar), sidebar_button(names[i], icons[i], path, app));
        g_free(path);
    }

    gtk_box_append(GTK_BOX(sidebar), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(sidebar), sidebar_button("Filesystem", "drive-harddisk-symbolic", "/", app));

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(sidebar), spacer);

    GtkWidget *hint = gtk_label_new(
        "Ctrl+L   Location\n"
        "Ctrl+F   Search\n"
        "Ctrl+R   Refresh\n"
        "Ctrl+C/X/V   Clipboard\n"
        "F2       Rename\n"
        "Delete   Trash\n"
        "Backspace Up"
    );
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_set_margin_start(hint, 10);
    gtk_box_append(GTK_BOX(sidebar), hint);

    return sidebar;
}

static void on_new_folder(GtkButton *button, gpointer user_data)
{
    (void)button;
    create_named(user_data, TRUE);
}

static void on_new_file(GtkButton *button, gpointer user_data)
{
    (void)button;
    create_named(user_data, FALSE);
}

static void on_copy(GtkButton *button, gpointer user_data)
{
    (void)button;
    copy_selection(user_data, FALSE);
}

static void on_cut(GtkButton *button, gpointer user_data)
{
    (void)button;
    copy_selection(user_data, TRUE);
}

static void on_paste(GtkButton *button, gpointer user_data)
{
    (void)button;
    paste_clipboard(user_data);
}

static void on_rename(GtkButton *button, gpointer user_data)
{
    (void)button;
    rename_selected(user_data);
}

static void on_delete(GtkButton *button, gpointer user_data)
{
    (void)button;
    delete_selected(user_data);
}

static GtkWidget *icon_button(const char *icon, const char *tooltip, GCallback callback, ForgeApp *app)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(button, tooltip);
    if (callback)
        g_signal_connect(button, "clicked", callback, app);
    return button;
}

static void build_window(ForgeApp *app)
{
    app->window = gtk_application_window_new(app->gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "ForgeFM");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1200, 760);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(toolbar, 8);
    gtk_widget_set_margin_bottom(toolbar, 8);
    gtk_widget_set_margin_start(toolbar, 8);
    gtk_widget_set_margin_end(toolbar, 8);

    gtk_box_append(GTK_BOX(toolbar), icon_button("go-previous-symbolic", "Back", G_CALLBACK(on_back), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("go-next-symbolic", "Forward", G_CALLBACK(on_forward), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("go-up-symbolic", "Up", G_CALLBACK(on_up), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("view-refresh-symbolic", "Reload", G_CALLBACK(on_reload), app));

    app->path_entry = gtk_entry_new();
    gtk_widget_set_hexpand(app->path_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->path_entry), "Enter a path…");
    gtk_box_append(GTK_BOX(toolbar), app->path_entry);

    app->search_entry = gtk_search_entry_new();
    gtk_widget_set_size_request(app->search_entry, 220, -1);
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->search_entry), "Search current folder");
    gtk_box_append(GTK_BOX(toolbar), app->search_entry);

    gtk_box_append(GTK_BOX(toolbar), icon_button("document-new-symbolic", "New file", G_CALLBACK(on_new_file), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("folder-new-symbolic", "New folder", G_CALLBACK(on_new_folder), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("edit-copy-symbolic", "Copy", G_CALLBACK(on_copy), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("edit-cut-symbolic", "Cut", G_CALLBACK(on_cut), app));

    app->paste_button = icon_button("edit-paste-symbolic", "Paste", G_CALLBACK(on_paste), app);
    gtk_widget_set_sensitive(app->paste_button, FALSE);
    gtk_box_append(GTK_BOX(toolbar), app->paste_button);

    gtk_box_append(GTK_BOX(toolbar), icon_button("document-edit-symbolic", "Rename (F2)", G_CALLBACK(on_rename), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("user-trash-symbolic", "Move to Trash", G_CALLBACK(on_delete), app));
    gtk_box_append(GTK_BOX(toolbar), icon_button("utilities-terminal-symbolic", "Open Terminal Here", NULL, NULL));

    GtkWidget *terminal_button = gtk_widget_get_last_child(toolbar);
    g_signal_connect_swapped(terminal_button, "clicked", G_CALLBACK(open_terminal_here), app);

    gtk_box_append(GTK_BOX(root), toolbar);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *body = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(body), 220);
    gtk_widget_set_vexpand(body, TRUE);
    gtk_paned_set_start_child(GTK_PANED(body), make_sidebar(app));

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(options, 6);
    gtk_widget_set_margin_bottom(options, 2);
    gtk_widget_set_margin_start(options, 8);
    gtk_widget_set_margin_end(options, 8);
    GtkWidget *hidden_label = gtk_label_new("Show hidden files");
    gtk_label_set_xalign(GTK_LABEL(hidden_label), 0.0f);
    app->hidden_toggle = gtk_switch_new();
    gtk_box_append(GTK_BOX(options), hidden_label);
    gtk_box_append(GTK_BOX(options), app->hidden_toggle);
    gtk_box_append(GTK_BOX(content), options);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    app->file_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->file_list), GTK_SELECTION_MULTIPLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(app->file_list), FALSE);
    gtk_widget_add_css_class(app->file_list, "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->file_list);
    gtk_widget_set_margin_top(scroller, 6);
    gtk_widget_set_margin_bottom(scroller, 8);
    gtk_widget_set_margin_start(scroller, 8);
    gtk_widget_set_margin_end(scroller, 8);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(content), scroller);

    gtk_paned_set_end_child(GTK_PANED(body), content);
    gtk_box_append(GTK_BOX(root), body);

    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(status, 5);
    gtk_widget_set_margin_bottom(status, 5);
    gtk_widget_set_margin_start(status, 10);
    gtk_widget_set_margin_end(status, 10);
    app->status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(app->status_label), 0.0f);
    gtk_widget_set_hexpand(app->status_label, TRUE);
    gtk_box_append(GTK_BOX(status), app->status_label);
    gtk_box_append(GTK_BOX(status), gtk_label_new("ForgeFM 0.2 MVP"));
    gtk_box_append(GTK_BOX(root), status);

    g_signal_connect(app->path_entry, "activate", G_CALLBACK(on_location_activate), app);
    g_signal_connect(app->search_entry, "search-changed", G_CALLBACK(on_search_changed), app);
    g_signal_connect(app->hidden_toggle, "notify::active", G_CALLBACK(on_hidden_toggled), app);
    g_signal_connect(app->file_list, "row-activated", G_CALLBACK(on_row_activated), app);

    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), app);
    gtk_widget_add_controller(app->window, keys);
}

ForgeApp *forge_app_new(GtkApplication *gtk_app)
{
    ForgeApp *app = g_new0(ForgeApp, 1);
    app->gtk_app = gtk_app;
    app->history = g_ptr_array_new_with_free_func(g_object_unref);
    app->clipboard = g_ptr_array_new_with_free_func((GDestroyNotify)clipboard_item_free);
    app->history_index = -1;
    build_window(app);
    return app;
}

void forge_app_activate(ForgeApp *app)
{
    gtk_window_present(GTK_WINDOW(app->window));
    if (!app->current_dir) {
        GFile *home = g_file_new_for_path(g_get_home_dir());
        navigate_to(app, home, TRUE);
        g_object_unref(home);
    }
}

void forge_app_free(ForgeApp *app)
{
    if (!app)
        return;
    g_clear_object(&app->current_dir);
    if (app->history)
        g_ptr_array_unref(app->history);
    if (app->clipboard)
        g_ptr_array_unref(app->clipboard);
    g_free(app->search_text);
    g_free(app);
}
