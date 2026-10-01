#ifndef FORGEFM_APP_H
#define FORGEFM_APP_H

#include <gtk/gtk.h>

typedef struct _ForgeApp ForgeApp;

ForgeApp *forge_app_new(GtkApplication *gtk_app);
void forge_app_activate(ForgeApp *app);
void forge_app_free(ForgeApp *app);

#endif
