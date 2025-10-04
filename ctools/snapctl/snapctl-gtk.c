#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SNAPCTL_ABS
#define SNAPCTL_ABS "/home/park/JKDBGVM/ctools/build/snapctl"
#endif

#ifndef SOCKET_PATH
#define SOCKET_PATH "/home/park/vm/win11/qmp.sock"
#endif

static void run_snapctl(const char *cmd, const char *arg) {
    char buf[1024];
    const char *socket_override = getenv("SNAPCTL_SOCKET");
    const char *socket_path = NULL;

    if (socket_override && *socket_override) {
        socket_path = socket_override;
    } else if (access(SOCKET_PATH, F_OK) == 0) {
        socket_path = SOCKET_PATH;
    }

    if (socket_path && access(socket_path, F_OK) == 0) {
        if (arg)
            snprintf(buf, sizeof(buf), "%s --socket %s %s %s", SNAPCTL_ABS, socket_path, cmd, arg);
        else
            snprintf(buf, sizeof(buf), "%s --socket %s %s", SNAPCTL_ABS, socket_path, cmd);
    } else {
        if (socket_path) {
            g_printerr("warning: socket '%s' not found, auto-detecting in snapctl.\n", socket_path);
        }
        if (arg)
            snprintf(buf, sizeof(buf), "%s %s %s", SNAPCTL_ABS, cmd, arg);
        else
            snprintf(buf, sizeof(buf), "%s %s", SNAPCTL_ABS, cmd);
    }

    system(buf); // 동기 실행
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
    if (name && *name) run_snapctl("deletevm", name);
}

static void on_list_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    run_snapctl("list", NULL);
}

static void on_compact_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    run_snapctl("compact", NULL);
}

static void on_pause_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    run_snapctl("pause", NULL);
}

static void on_run_clicked(GtkWidget *widget, gpointer data) {
    (void)widget; (void)data;
    run_snapctl("run", NULL);
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

    GtkWidget *btn_save    = gtk_button_new_with_label("Save Snapshot");
    GtkWidget *btn_load    = gtk_button_new_with_label("Load Snapshot");
    GtkWidget *btn_del     = gtk_button_new_with_label("Delete Snapshot");
    GtkWidget *btn_list    = gtk_button_new_with_label("List Snapshots");
    GtkWidget *btn_pause   = gtk_button_new_with_label("Pause VM");
    GtkWidget *btn_run     = gtk_button_new_with_label("Run VM");
    GtkWidget *btn_compact = gtk_button_new_with_label("Compact Disk");

    gtk_grid_attach(GTK_GRID(grid), btn_save, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_load, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_del,  0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_list, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_pause, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_run,   1, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_compact, 0, 4, 2, 1);

    g_signal_connect(btn_save, "clicked", G_CALLBACK(on_save_clicked), entry);
    g_signal_connect(btn_load, "clicked", G_CALLBACK(on_load_clicked), entry);
    g_signal_connect(btn_del,  "clicked", G_CALLBACK(on_del_clicked), entry);
    g_signal_connect(btn_list, "clicked", G_CALLBACK(on_list_clicked), NULL);
    g_signal_connect(btn_pause, "clicked", G_CALLBACK(on_pause_clicked), NULL);
    g_signal_connect(btn_run,   "clicked", G_CALLBACK(on_run_clicked), NULL);
    g_signal_connect(btn_compact, "clicked", G_CALLBACK(on_compact_clicked), NULL);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
