#include "panda_plugin.h"
#include "qemu-common.h"
#include "qdict.h"
#include "qmp-commands.h"
#include "hmp.h"
#include "error.h"

#include <dlfcn.h>
#include <string.h>

// WARNING: this is all gloriously un-thread-safe

// Array of pointers to PANDA callback lists, one per callback type
panda_cb_list *panda_cbs[PANDA_CB_LAST];

panda_plugin panda_plugins[MAX_PANDA_PLUGINS];
int nb_panda_plugins;

bool panda_load_plugin(const char *filename) {
    void *plugin = dlopen(filename, RTLD_NOW);
    if(!plugin) {
        fprintf(stderr, "Failed to load %s: %s\n", filename, dlerror());
        return false;
    }
    bool (*init_fn)(void *) = dlsym(plugin, "init_plugin");
    if(!init_fn) {
        fprintf(stderr, "Couldn't get symbol %s: %s\n", "init_plugin", dlerror());
        dlclose(plugin);
        return false;
    }
    if(init_fn(plugin)) {
        panda_plugins[nb_panda_plugins].plugin = plugin;
        strncpy(panda_plugins[nb_panda_plugins].name, basename(filename), 256);
        nb_panda_plugins++;
        return true;
    }
    else {
        dlclose(plugin);
        return false;
    }
}

// Internal: remove a plugin from the global array
static void panda_delete_plugin(int i) {
    if (i != nb_panda_plugins - 1) { // not the last element
        memmove(&panda_plugins[i], &panda_plugins[i+1], (nb_panda_plugins - i - 1)*sizeof(panda_plugin));
    }
    nb_panda_plugins--;
}

void panda_unload_plugin(int plugin_idx) {
    void *plugin = panda_plugins[plugin_idx].plugin;
    void (*uninit_fn)(void *) = dlsym(plugin, "uninit_plugin");
    if(!uninit_fn) {
        fprintf(stderr, "Couldn't get symbol %s: %s\n", "uninit_plugin", dlerror());
    }
    else {
        uninit_fn(plugin);
    }
    panda_unregister_callbacks(plugin);
    panda_delete_plugin(plugin_idx);
    dlclose(plugin);
}

void panda_unload_plugins(void) {
    // Unload them starting from the end to avoid having to shuffle everything
    // down each time
    while (nb_panda_plugins > 0) {
        panda_unload_plugin(nb_panda_plugins - 1);
    }
}

void * panda_get_plugin_by_name(const char *plugin_name) {
    int i;
    for (i = 0; i < nb_panda_plugins; i++) {
        if (strncmp(panda_plugins[i].name, plugin_name, 256) == 0)
            return panda_plugins[i].plugin;
    }
    return NULL;
}

void panda_register_callback(void *plugin, panda_cb_type type, panda_cb cb) {
    panda_cb_list *new_list = g_new0(panda_cb_list,1);
    new_list->entry = cb;
    new_list->owner = plugin;
    new_list->prev = NULL;
    if(panda_cbs[type] != NULL) {
        new_list->next = panda_cbs[type];
        panda_cbs[type]->prev = new_list;
    }
    panda_cbs[type] = new_list;
}

void panda_unregister_callbacks(void *plugin) {
    // Remove callbacks
    int i;
    for (i = 0; i < PANDA_CB_LAST; i++) {
        panda_cb_list *plist;
        plist = panda_cbs[i];
        while(plist != NULL) {
            if (plist->owner == plugin) {
                panda_cb_list *old_plist = plist;
                // Unlink
                if (plist->prev)
                    plist->prev->next = plist->next;
                if (plist->next)
                    plist->next->prev = plist->prev;
                // Advance the pointer
                plist = plist->next;
                // Free the entry we just unlinked
                g_free(old_plist);
            }
            else {
                plist = plist->next;
            }
        }
    }
}

// QMP

void qmp_load_plugin(const char *filename, Error **errp) {
    if(!panda_load_plugin(filename)) {
        // TODO: do something with errp here?
    }
}

void qmp_unload_plugin(int64_t index, Error **errp) {
    if (index >= nb_panda_plugins || index < 0) {
        // TODO: errp
    }
    panda_unload_plugin(index);
}

void qmp_list_plugins(Error **errp) {
    
}

// HMP
void hmp_panda_load_plugin(Monitor *mon, const QDict *qdict) {
    Error *err;
    const char *filename = qdict_get_try_str(qdict, "filename");
    qmp_load_plugin(filename, &err);
}

void hmp_panda_unload_plugin(Monitor *mon, const QDict *qdict) {
    Error *err;
    const int index = qdict_get_try_int(qdict, "index", -1);
    qmp_unload_plugin(index, &err);
}

void hmp_panda_list_plugins(Monitor *mon, const QDict *qdict) {
    Error *err;
    int i;
    monitor_printf(mon, "idx\t%-20s\taddr\n", "name");
    for (i = 0; i < nb_panda_plugins; i++) {
        monitor_printf(mon, "%d\t%-20s\t%p\n", i, panda_plugins[i].name, panda_plugins[i].plugin);
    }
    qmp_list_plugins(&err);
}
