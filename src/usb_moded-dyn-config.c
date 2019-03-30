/**
 * @file usb_moded-dyn-mode.c
 *
 * Copyright (C) 2011 Nokia Corporation. All rights reserved.
 * Copyright (C) 2013-2019 Jolla. All rights reserved.
 *
 * @author: Philippe De Swert <philippe.de-swert@nokia.com>
 * @author: Philippe De Swert <philippedeswert@gmail.com>
 * @author: Philippe De Swert <philippe.deswert@jollamobile.com>
 * @author: Thomas Perl <thomas.perl@jolla.com>
 * @author: Slava Monich <slava.monich@jolla.com>
 * @author: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the Lesser GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the Lesser GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "usb_moded-dyn-config.h"

#include "usb_moded-log.h"

#include <stdlib.h>

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * MODEDATA
 * ------------------------------------------------------------------------- */

void               modedata_free   (modedata_t *list_item);
static gint        modedata_sort_cb(gconstpointer a, gconstpointer b);
static modedata_t *modedata_load   (const gchar *filename);

/* ------------------------------------------------------------------------- *
 * MODELIST
 * ------------------------------------------------------------------------- */

void   modelist_free(GList *modelist);
GList *modelist_load(int diag);

/* ========================================================================= *
 * MODEDATA
 * ========================================================================= */

/** Relase modedata_t object
 *
 * @param list_item Object pointer, or NULL
 */
void
modedata_free(modedata_t *list_item)
{
    LOG_REGISTER_CONTEXT;

    if( list_item ) {
        free(list_item->mode_name);
        free(list_item->mode_module);
        free(list_item->network_interface);
        free(list_item->sysfs_path);
        free(list_item->sysfs_value);
        free(list_item->sysfs_reset_value);
        free(list_item->android_extra_sysfs_path);
        free(list_item->android_extra_sysfs_value);
        free(list_item->android_extra_sysfs_path2);
        free(list_item->android_extra_sysfs_value2);
        free(list_item->android_extra_sysfs_path3);
        free(list_item->android_extra_sysfs_value3);
        free(list_item->android_extra_sysfs_path4);
        free(list_item->android_extra_sysfs_value4);
        free(list_item->idProduct);
        free(list_item->idVendorOverride);
#ifdef CONNMAN
        free(list_item->connman_tethering);
#endif
        free(list_item);
    }
}

/** Callback for sorting mode list alphabetically
 *
 * For use with g_list_sort()
 *
 * @param a  Object pointer
 * @param b  Object pointer
 *
 * @return result of comparing object names
 */
static gint
modedata_sort_cb(gconstpointer a, gconstpointer b)
{
    LOG_REGISTER_CONTEXT;

    modedata_t *aa = (modedata_t *)a;
    modedata_t *bb = (modedata_t *)b;

    return g_strcmp0(aa->mode_name, bb->mode_name);
}

/** Load mode data from file
 *
 * @param filename  Path to file from which to read
 *
 * @return Mode data object, or NULL
 */
static modedata_t *
modedata_load(const gchar *filename)
{
    LOG_REGISTER_CONTEXT;

    bool success = false;
    GKeyFile *settingsfile = g_key_file_new();
    modedata_t *list_item = NULL;

    if( !g_key_file_load_from_file(settingsfile, filename, G_KEY_FILE_NONE, NULL) ) {
        log_err("%s: can't read mode configuration file", filename);
        goto EXIT;
    }

    list_item = calloc(1, sizeof *list_item);

    // [MODE_ENTRY = "mode"]
    list_item->mode_name         = g_key_file_get_string(settingsfile, MODE_ENTRY, MODE_NAME_KEY, NULL);
    list_item->mode_module       = g_key_file_get_string(settingsfile, MODE_ENTRY, MODE_MODULE_KEY, NULL);

    log_debug("Dynamic mode name = %s\n", list_item->mode_name);
    log_debug("Dynamic mode module = %s\n", list_item->mode_module);

    list_item->appsync           = g_key_file_get_integer(settingsfile, MODE_ENTRY, MODE_NEEDS_APPSYNC_KEY, NULL);
    list_item->mass_storage      = g_key_file_get_integer(settingsfile, MODE_ENTRY, MODE_MASS_STORAGE_KEY, NULL);
    list_item->network           = g_key_file_get_integer(settingsfile, MODE_ENTRY, MODE_NETWORK_KEY, NULL);
    list_item->network_interface = g_key_file_get_string(settingsfile, MODE_ENTRY, MODE_NETWORK_INTERFACE_KEY, NULL);

    // [MODE_OPTIONS_ENTRY = "options"]
    list_item->sysfs_path                 = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_SYSFS_PATH, NULL);
    list_item->sysfs_value                = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_SYSFS_VALUE, NULL);
    list_item->sysfs_reset_value          = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_SYSFS_RESET_VALUE, NULL);

    list_item->android_extra_sysfs_path   = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_PATH, NULL);
    list_item->android_extra_sysfs_path2  = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_PATH2, NULL);
    list_item->android_extra_sysfs_path3  = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_PATH3, NULL);
    list_item->android_extra_sysfs_path4  = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_PATH4, NULL);
    list_item->android_extra_sysfs_value  = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_VALUE, NULL);
    list_item->android_extra_sysfs_value2 = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_VALUE2, NULL);
    list_item->android_extra_sysfs_value3 = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_VALUE3, NULL);
    list_item->android_extra_sysfs_value4 = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_ANDROID_EXTRA_SYSFS_VALUE4, NULL);

    list_item->idProduct                  = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_IDPRODUCT, NULL);
    list_item->idVendorOverride           = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_IDVENDOROVERRIDE, NULL);
    list_item->nat                        = g_key_file_get_integer(settingsfile, MODE_OPTIONS_ENTRY, MODE_HAS_NAT, NULL);
    list_item->dhcp_server                = g_key_file_get_integer(settingsfile, MODE_OPTIONS_ENTRY, MODE_HAS_DHCP_SERVER, NULL);
#ifdef CONNMAN
    list_item->connman_tethering          = g_key_file_get_string(settingsfile, MODE_OPTIONS_ENTRY, MODE_CONNMAN_TETHERING, NULL);
#endif

    //log_debug("Dynamic mode sysfs path = %s\n", list_item->sysfs_path);
    //log_debug("Dynamic mode sysfs value = %s\n", list_item->sysfs_value);
    //log_debug("Android extra mode sysfs path2 = %s\n", list_item->android_extra_sysfs_path2);
    //log_debug("Android extra value2 = %s\n", list_item->android_extra_sysfs_value2);

    if( list_item->mode_name == NULL || list_item->mode_module == NULL ) {
        log_err("%s: mode_name or mode_module not defined", filename);
        goto EXIT;
    }

    if( list_item->network && list_item->network_interface == NULL) {
        log_err("%s: network not fully defined", filename);
        goto EXIT;
    }

    if( (list_item->sysfs_path && !list_item->sysfs_value) ||
        (list_item->sysfs_reset_value && !list_item->sysfs_path) ) {
        /* In theory all of this is optional.
         *
         * In most cases 'sysfs_value' holds a list of functions to enable,
         * and 'sysfs_path' or 'sysfs_reset_value' values are simply ignored.
         *
         * However, for the benefit of existing special configuration files
         * like the one for host mode:
         * - having sysfs_path implies that sysfs_value should be set too
         * - having sysfs_reset_value implies that sysfs_path should be set
         */
        log_err("%s: sysfs_value not fully defined", filename);
        goto EXIT;
    }

    log_debug("%s: successfully loaded", filename);
    success = true;

EXIT:
    g_key_file_free(settingsfile);

    if( !success )
        modedata_free(list_item), list_item = 0;

    return list_item;
}

/* ========================================================================= *
 * MODELIST
 * ========================================================================= */

/** Release mode list
 *
 * @param modelist List pointer, or NULL
 */
void
modelist_free(GList *modelist)
{
    LOG_REGISTER_CONTEXT;

    if(modelist)
    {
        g_list_foreach(modelist, (GFunc) modedata_free, NULL);
        g_list_free(modelist);
        modelist = 0;
    }
}

/** Load mode data files from configuration directory
 *
 * @param diag  true to load diagnostic modes, false for normal modes
 *
 * @return List of mode data objects, or NULL
 */
GList *
modelist_load(int diag)
{
    LOG_REGISTER_CONTEXT;

    GDir *confdir;
    GList *modelist = NULL;
    const gchar *dirname;
    modedata_t *list_item;
    gchar *full_filename = NULL;

    if(diag)
        confdir = g_dir_open(DIAG_DIR_PATH, 0, NULL);
    else
        confdir = g_dir_open(MODE_DIR_PATH, 0, NULL);
    if(confdir)
    {
        while((dirname = g_dir_read_name(confdir)) != NULL)
        {
            log_debug("Read file %s\n", dirname);
            if(diag)
                full_filename = g_strconcat(DIAG_DIR_PATH, "/", dirname, NULL);
            else
                full_filename = g_strconcat(MODE_DIR_PATH, "/", dirname, NULL);
            list_item = modedata_load(full_filename);
            /* free full_filename immediately as we do not use it anymore */
            free(full_filename);
            if(list_item)
                modelist = g_list_append(modelist, list_item);
        }
        g_dir_close(confdir);
    }
    else
        log_debug("Mode confdir open failed or file is incomplete/invalid.\n");

    modelist = g_list_sort (modelist, modedata_sort_cb);
    return modelist;
}
