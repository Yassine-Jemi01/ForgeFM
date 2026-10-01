#include "app.h"

static void on_activate(GtkApplication *gtk_app, gpointer user_data)
{
    ForgeApp **holder = user_data;
    if (!*holder)
        *holder = forge_app_new(gtk_app);
    forge_app_activate(*holder);
}

static void on_shutdown(GApplication *application, gpointer user_data)
{
    (void)application;
    ForgeApp **holder = user_data;
    forge_app_free(*holder);
    *holder = NULL;
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("io.yassine.ForgeFM", G_APPLICATION_DEFAULT_FLAGS);
    ForgeApp *forge = NULL;

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &forge);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &forge);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
