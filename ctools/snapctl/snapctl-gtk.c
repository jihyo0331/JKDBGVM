#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#ifndef SNAPCTL_ABS
#define SNAPCTL_ABS "/home/didqls/ws/qemu/ctools/build/snapctl"
#endif

#ifndef SOCKET_PATH
#define SOCKET_PATH "/home/didqls/vm/win11/qmp.sock"
#endif

static void run_snapctl(const char *cmd, const char *arg) {
    char buf[1024];
    if (arg)
        snprintf(buf, sizeof(buf), "%s --socket %s %s %s", SNAPCTL_ABS, SOCKET_PATH, cmd, arg);
    else
        snprintf(buf, sizeof(buf), "%s --socket %s %s", SNAPCTL_ABS, SOCKET_PATH, cmd);
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
