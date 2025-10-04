#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#ifdef SNAPCTL
#define SNAPCTL_ABS SNAPCTL
#endif

#ifndef SNAPCTL_ABS
#define SNAPCTL_ABS "/home/didqls/ws/qemu/ctools/build/snapctl"
#endif

#ifndef SOCKET_PATH
#define SOCKET_PATH "/home/didqls/vm/win11/qmp.sock"
#endif

#ifndef SNAPCTL_TIMELOG_PATH
#define SNAPCTL_TIMELOG_PATH ""
#endif

#ifndef SNAPCTL_SNAPSHOT_DIR
#define SNAPCTL_SNAPSHOT_DIR ""
#endif

#ifndef SNAPCTL_BLOCK_MIGRATION_DEFAULT
#define SNAPCTL_BLOCK_MIGRATION_DEFAULT 0
#endif

static gboolean is_truthy(const char *value) {
    if (!value) {
        return FALSE;
    }
    return g_ascii_strcasecmp(value, "1") == 0 ||
           g_ascii_strcasecmp(value, "true") == 0 ||
           g_ascii_strcasecmp(value, "yes") == 0 ||
           g_ascii_strcasecmp(value, "on") == 0;
}

static void append_optional_arg(GString *cmdline, const char *option, const char *value) {
    if (!value || !*value) {
        return;
    }
    gchar *quoted = g_shell_quote(value);
    g_string_append_printf(cmdline, " %s %s", option, quoted);
    g_free(quoted);
}

static void run_snapctl(const char *cmd, const char *arg) {
    const char *timelog = getenv("SNAPCTL_TIMELOG");
    if ((!timelog || !*timelog) && SNAPCTL_TIMELOG_PATH[0] != '\0') {
        timelog = SNAPCTL_TIMELOG_PATH;
    }

    const char *snapdir = getenv("SNAPCTL_SNAPSHOT_DIR");
    if ((!snapdir || !*snapdir) && SNAPCTL_SNAPSHOT_DIR[0] != '\0') {
        snapdir = SNAPCTL_SNAPSHOT_DIR;
    }

    gboolean block_migration = SNAPCTL_BLOCK_MIGRATION_DEFAULT;
    const char *block_env = getenv("SNAPCTL_BLOCK_MIGRATION");
    if (block_env) {
        block_migration = is_truthy(block_env);
    }

    GString *cmdline = g_string_new(NULL);
    gchar *snapctl_quoted = g_shell_quote(SNAPCTL_ABS);
    gchar *socket_quoted = g_shell_quote(SOCKET_PATH);

    g_string_append_printf(cmdline, "%s --socket %s", snapctl_quoted, socket_quoted);

    append_optional_arg(cmdline, "--timelog", timelog);
    append_optional_arg(cmdline, "--snapshot-dir", snapdir);
    if (block_migration) {
        g_string_append(cmdline, " --block-migration");
    }

    g_free(snapctl_quoted);
    g_free(socket_quoted);

    g_string_append_printf(cmdline, " %s", cmd);

    if (arg && *arg) {
        gchar *arg_quoted = g_shell_quote(arg);
        g_string_append_printf(cmdline, " %s", arg_quoted);
        g_free(arg_quoted);
    }

    system(cmdline->str); // 동기 실행
    g_string_free(cmdline, TRUE);
}

static void on_save_clicked(GtkWidget *widget, gpointer entry) {
    (void)widget;
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
    if (name && *name) run_snapctl("savevm", name);
}

static void on_load_clicked(GtkWidget *widget, gpointer entry) {
    (void)widget;
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
    if (name && *name) {
        run_snapctl("loadvm", name);  

    }
}

static void on_del_clicked(GtkWidget *widget, gpointer entry) {
    (void)widget;
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry));
    if (name && *name) run_snapctl("delvm", name);
}

static void on_list_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    run_snapctl("list", NULL);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "QEMU Snapshot Manager");
    gtk_window_set_default_size(GTK_WINDOW(window), 300, 200);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_container_add(GTK_CONTAINER(window), grid);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Snapshot name");
    gtk_grid_attach(GTK_GRID(grid), entry, 0, 0, 2, 1);

    GtkWidget *btn_save = gtk_button_new_with_label("Save Snapshot");
    GtkWidget *btn_load = gtk_button_new_with_label("Load Snapshot");
    GtkWidget *btn_del  = gtk_button_new_with_label("Delete Snapshot");
    GtkWidget *btn_list = gtk_button_new_with_label("List Snapshots");

    gtk_grid_attach(GTK_GRID(grid), btn_save, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_load, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_del,  0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_list, 1, 2, 1, 1);

    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save_clicked), entry);
    g_signal_connect(btn_load, "clicked", G_CALLBACK(on_load_clicked), entry);
    g_signal_connect(btn_del,  "clicked", G_CALLBACK(on_del_clicked), entry);
    g_signal_connect(btn_list, "clicked", G_CALLBACK(on_list_clicked), NULL);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
