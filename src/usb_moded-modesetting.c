/**
 * @file usb_moded-modesetting.c
 *
 * Copyright (C) 2010 Nokia Corporation. All rights reserved.
 * Copyright (C) 2013-2018 Jolla Ltd.
 *
 * @author: Philippe De Swert <philippe.de-swert@nokia.com>
 * @author: Philippe De Swert <phdeswer@lumi.maa>
 * @author: Philippe De Swert <philippedeswert@gmail.com>
 * @author: Philippe De Swert <philippe.deswert@jollamobile.com>
 * @author: Bernd Wachter <bernd.wachter@jollamobile.com>
 * @author: Slava Monich <slava.monich@jolla.com>
 * @author: Thomas Perl <m@thp.io>
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

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <limits.h>

#include <glib.h>

#include "usb_moded.h"
#include "usb_moded-modules.h"
#include "usb_moded-modes.h"
#include "usb_moded-log.h"
#include "usb_moded-dbus.h"
#include "usb_moded-dbus-private.h"
#include "usb_moded-appsync.h"
#include "usb_moded-config-private.h"
#include "usb_moded-modesetting.h"
#include "usb_moded-network.h"
#include "usb_moded-android.h"
#include "usb_moded-configfs.h"

/* ========================================================================= *
 * Prototypes
 * ========================================================================= */

/* -- modesetting -- */

static void      modesetting_track_value                (const char *path, const char *text);
void             modesetting_verify_values              (void);
static char     *modesetting_strip                      (char *str);
static char     *modesetting_read_from_file             (const char *path, size_t maxsize);
int              modesetting_write_to_file_real         (const char *file, int line, const char *func, const char *path, const char *text);
static gboolean  modesetting_network_retry              (gpointer data);
static int       modesetting_set_mass_storage_mode      (struct mode_list_elem *data);
static int       modesetting_unset_mass_storage_mode    (struct mode_list_elem *data);
static void      modesetting_report_mass_storage_blocker(const char *mountpoint, int try);
int              modesetting_set_dynamic_mode           (void);
static void      modesetting_unset_dynamic_mode         (void);
int              modesetting_cleanup                    (const char *module);
void             modesetting_init                       (void);
void             modesetting_quit                       (void);

/* ========================================================================= *
 * Data
 * ========================================================================= */

static GHashTable *tracked_values = 0;
static guint delayed_network = 0;

/* ========================================================================= *
 * Functions
 * ========================================================================= */

static void modesetting_track_value(const char *path, const char *text)
{
    if( !tracked_values || !path )
        goto EXIT;

    if( text )
        g_hash_table_replace(tracked_values, g_strdup(path), g_strdup(text));
    else
        g_hash_table_remove(tracked_values, path);

EXIT:
    return;
}

void modesetting_verify_values(void)
{
    GHashTableIter iter;
    gpointer key, value;

    if( !tracked_values )
        goto EXIT;

    g_hash_table_iter_init(&iter, tracked_values);
    while( g_hash_table_iter_next(&iter, &key, &value) )
    {
        const char *path = key;
        const char *text = value;

        char *curr = modesetting_read_from_file(path, 0x1000);

        if( g_strcmp0(text, curr) ) {
            /* There might be case mismatch between hexadecimal
             * values used in configuration files vs what we get
             * back when reading from kernel interfaces. */
            if( text && curr && !g_ascii_strcasecmp(text, curr) ) {
                log_debug("unexpected change '%s' : '%s' -> '%s' (case diff only)", path,
                          text ?: "???",
                          curr ?: "???");
            }
            else {
                log_warning("unexpected change '%s' : '%s' -> '%s'", path,
                            text ?: "???",
                            curr ?: "???");
            }
            modesetting_track_value(path, curr);
        }

        free(curr);
    }

EXIT:
    return;
}

static char *modesetting_strip(char *str)
{
    unsigned char *src = (unsigned char *)str;
    unsigned char *dst = (unsigned char *)str;

    while( *src > 0 && *src <= 32 ) ++src;

    for( ;; )
    {
        while( *src > 32 ) *dst++ = *src++;
        while( *src > 0 && *src <= 32 ) ++src;
        if( *src == 0 ) break;
        *dst++ = ' ';
    }
    *dst = 0;
    return str;
}

static char *modesetting_read_from_file(const char *path, size_t maxsize)
{
    int      fd   = -1;
    ssize_t  done = 0;
    char    *data = 0;
    char    *text = 0;

    if((fd = open(path, O_RDONLY)) == -1)
    {
        /* Silently ignore things that could result
         * from missing / read-only files */
        if( errno != ENOENT && errno != EACCES )
            log_warning("%s: open: %m", path);
        goto cleanup;
    }

    if( !(data = malloc(maxsize + 1)) )
        goto cleanup;

    if((done = read(fd, data, maxsize)) == -1)
    {
        log_warning("%s: read: %m", path);
        goto cleanup;
    }

    text = realloc(data, done + 1), data = 0;
    text[done] = 0;
    modesetting_strip(text);

cleanup:
    free(data);
    if(fd != -1) close(fd);
    return text;
}

int modesetting_write_to_file_real(const char *file, int line, const char *func,
                                   const char *path, const char *text)
{
    int err = -1;
    int fd = -1;
    size_t todo = 0;
    char *prev = 0;
    bool  clear = false;

    /* if either path or the text to be written are not there
     we return an error */
    if(!text || !path)
        return err;

    /* When attempting to clear ffs function list, writing an
     * empty string is ignored and accomplishes nothing - while
     * writing non-existing function clears the list but returns
     * write error.
     *
     * Treat "none" (which is used as place-holder value in both
     * configuration files and usb-moded sources) and "" similarly:
     * - Write invalid function name to sysfs
     * - Ignore resulting write error under default logging level
     * - Assume reading from sysfs will result in empty string
     */
    if( !strcmp(path, ANDROID0_FUNCTIONS) ) {
        if( !strcmp(text, "") || !strcmp(text, "none") ) {
            text = "none";
            clear = true;
        }
    }

    /* If the file can be read, it also means we can later check that
     * the file retains the value we are about to write here. */
    if( (prev = modesetting_read_from_file(path, 0x1000)) )
        modesetting_track_value(path, clear ? "" : text);

    log_debug("%s:%d: %s(): WRITE '%s' : '%s' --> '%s'",
              file, line, func,
              path, prev ?: "???", text);

    todo  = strlen(text);

    /* no O_CREAT -> writes only to already existing files */
    if( (fd = TEMP_FAILURE_RETRY(open(path, O_WRONLY))) == -1 )
    {
        log_warning("open(%s): %m", path);
        goto cleanup;
    }

    while( todo > 0 )
    {
        ssize_t n = TEMP_FAILURE_RETRY(write(fd, text, todo));
        if( n < 0 )
        {
            if( clear && errno == EINVAL )
                log_debug("write(%s): %m (expected failure)", path);
            else
                log_warning("write(%s): %m", path);
            goto cleanup;
        }
        todo -= n;
        text += n;
    }

    err = 0;

cleanup:

    if( fd != -1 ) TEMP_FAILURE_RETRY(close(fd));

    free(prev);

    return err;
}

static gboolean modesetting_network_retry(gpointer data)
{
    delayed_network = 0;
    network_up(data);
    return(FALSE);
}

static int modesetting_set_mass_storage_mode(struct mode_list_elem *data)
{
    gchar *command;
    char command2[256], *real_path = NULL, *mountpath;
    char *mount;
    gchar **mounts;
    int ret = 0, i = 0, mountpoints = 0, fua = 0, try = 0;

    /* send unmount signal so applications can release their grasp on the fs, do this here so they have time to act */
    umdbus_send_state_signal(USB_PRE_UNMOUNT);
    fua = config_find_sync();
    mount = config_find_mounts();
    if(mount)
    {
        mounts = g_strsplit(mount, ",", 0);
        /* check amount of mountpoints */
        for(i=0 ; mounts[i] != NULL; i++)
        {
            mountpoints++;
        }

        if(strcmp(data->mode_module, MODULE_NONE))
        {
            /* check if the file storage module has been loaded with sufficient luns in the parameter,
             if not, unload and reload or load it. Since  mountpoints start at 0 the amount of them is one more than their id */
            sprintf(command2, "/sys/devices/platform/musb_hdrc/gadget/gadget-lun%d/file", (mountpoints - 1) );
            if(access(command2, R_OK) == -1)
            {
                log_debug("%s does not exist, unloading and reloading mass_storage\n", command2);
                modules_unload_module(MODULE_MASS_STORAGE);
                sprintf(command2, "modprobe %s luns=%d \n", MODULE_MASS_STORAGE, mountpoints);
                log_debug("usb-load command = %s \n", command2);
                ret = usbmoded_system(command2);
                if(ret)
                    return(ret);
            }
        }
        /* umount filesystems */
        for(i=0 ; mounts[i] != NULL; i++)
        {
            /* check if filesystem is mounted or not, if ret = 1 it is already unmounted */
            real_path = realpath(mounts[i], NULL);
            if(real_path)
                mountpath = real_path;
            else
                mountpath = mounts[i];
umount:                 command = g_strconcat("mount | grep ", mountpath, NULL);
            ret = usbmoded_system(command);
            g_free(command);
            if(!ret)
            {
                /* no check for / needed as that will fail to umount anyway */
                command = g_strconcat("umount ", mountpath, NULL);
                log_debug("unmount command = %s\n", command);
                ret = usbmoded_system(command);
                g_free(command);
                if(ret != 0)
                {
                    if(try != 3)
                    {
                        try++;
                        usbmoded_sleep(1);
                        log_err("Umount failed. Retrying\n");
                        modesetting_report_mass_storage_blocker(mount, 1);
                        goto umount;
                    }
                    else
                    {
                        log_err("Unmounting %s failed\n", mount);
                        modesetting_report_mass_storage_blocker(mount, 2);
                        umdbus_send_error_signal(UMOUNT_ERROR);
                        return(ret);
                    }
                }
            }
            else
                /* already unmounted. Set return value to 0 since there is no error */
                ret = 0;
        }

        /* activate mounts after sleeping 1s to be sure enumeration happened and autoplay will work in windows*/
        usbmoded_sleep(1);
        for(i=0 ; mounts[i] != NULL; i++)
        {

            if(strcmp(data->mode_module, MODULE_NONE))
            {
                sprintf(command2, "echo %i  > /sys/devices/platform/musb_hdrc/gadget/gadget-lun%d/nofua", fua, i);
                log_debug("usb lun = %s active\n", command2);
                usbmoded_system(command2);
                sprintf(command2, "/sys/devices/platform/musb_hdrc/gadget/gadget-lun%d/file", i);
                log_debug("usb lun = %s active\n", command2);
                write_to_file(command2, mounts[i]);
            }
            else
            {
                write_to_file(ANDROID0_ENABLE, "0");
                write_to_file(ANDROID0_FUNCTIONS, "mass_storage");
                //write_to_file("/sys/class/android_usb/f_mass_storage/lun/nofua", fua);
                write_to_file("/sys/class/android_usb/f_mass_storage/lun/file", mount);
                write_to_file(ANDROID0_ENABLE, "1");

            }
        }
        g_strfreev(mounts);
        g_free(mount);
        if(real_path)
            free(real_path);
    }

    /* only send data in use signal in case we actually succeed */
    if(!ret)
        umdbus_send_state_signal(DATA_IN_USE);

    return(ret);

}

static int modesetting_unset_mass_storage_mode(struct mode_list_elem *data)
{
    gchar *command;
    char command2[256], *real_path = NULL, *mountpath;
    char *mount;
    gchar **mounts;
    int ret = 1, i = 0;

    mount = config_find_mounts();
    if(mount)
    {
        mounts = g_strsplit(mount, ",", 0);
        for(i=0 ; mounts[i] != NULL; i++)
        {
            /* check if it is still or already mounted, if so (ret==0) skip mounting */
            real_path = realpath(mounts[i], NULL);
            if(real_path)
                mountpath = real_path;
            else
                mountpath = mounts[i];
            command = g_strconcat("mount | grep ", mountpath, NULL);
            ret = usbmoded_system(command);
            g_free(command);
            if(ret)
            {
                command = g_strconcat("mount ", mountpath, NULL);
                log_debug("mount command = %s\n",command);
                ret = usbmoded_system(command);
                g_free(command);
                /* mount returns 0 if success */
                if(ret != 0 )
                {
                    log_err("Mounting %s failed\n", mount);
                    if(ret)
                    {
                        g_free(mount);
                        mount = config_find_alt_mount();
                        if(mount)
                        {
                            command = g_strconcat("mount -t tmpfs tmpfs -o ro --size=512K ", mount, NULL);
                            log_debug("Total failure, mount ro tmpfs as fallback\n");
                            ret = usbmoded_system(command);
                            g_free(command);
                        }
                        umdbus_send_error_signal(RE_MOUNT_FAILED);
                    }

                }
            }
            if(data != NULL)
            {
                if(!strcmp(data->mode_module, MODULE_NONE))
                {
                    log_debug("Disable android mass storage\n");
                    write_to_file("/sys/class/android_usb/f_mass_storage/lun/file", "0");
                    write_to_file(ANDROID0_ENABLE, "0");
                }
            }
            else
            {
                sprintf(command2, "echo \"\"  > /sys/devices/platform/musb_hdrc/gadget/gadget-lun%d/file", i);
                log_debug("usb lun = %s inactive\n", command2);
                usbmoded_system(command2);
            }
        }
        g_strfreev(mounts);
        g_free(mount);
        if(real_path)
            free(real_path);
    }

    return(ret);

}

static void modesetting_report_mass_storage_blocker(const char *mountpoint, int try)
{
    FILE *stream = 0;
    gchar *lsof_command = 0;
    int count = 0;

    lsof_command = g_strconcat("lsof ", mountpoint, NULL);

    if( (stream = usbmoded_popen(lsof_command, "r")) )
    {
        char *text = 0;
        size_t size = 0;

        while( getline(&text, &size, stream) >= 0 )
        {
            /* skip the first line as it does not contain process info */
            if(count != 0)
            {
                gchar **split = 0;
                split = g_strsplit((const gchar*)text, " ", 2);
                log_err("Mass storage blocked by process %s\n", split[0]);
                umdbus_send_error_signal(split[0]);
                g_strfreev(split);
            }
            count++;
        }
        pclose(stream);
    }
    g_free(lsof_command);
    if(try == 2)
        log_err("Setting Mass storage blocked. Giving up.\n");

}

int modesetting_set_dynamic_mode(void)
{
    struct mode_list_elem *data;
    int ret = 1;
    int network = 1;

    data = usbmoded_get_usb_mode_data();

    if(!data)
        return(ret);

    if(data->mass_storage)
    {
        return modesetting_set_mass_storage_mode(data);
    }

#ifdef APP_SYNC
    if(data->appsync)
        if(appsync_activate_sync(data->mode_name)) /* returns 1 on error */
        {
            log_debug("Appsync failure");
            return(ret);
        }
#endif
    if( configfs_in_use() ) {
        /* Configfs based gadget configuration */
        char *id = config_get_android_vendor_id();
        configfs_set_udc(false);
        configfs_set_productid(data->idProduct);
        configfs_set_vendorid(data->idVendorOverride ?: id);
        configfs_set_function(data->sysfs_value);
        ret = configfs_set_udc(true) ? 0 : -1;
        free(id);
    }
    else if( android_in_use() ) {
        /* Android USB based gadget configuration */

        /* make sure things are disabled before changing functionality */
        write_to_file(data->softconnect_path, data->softconnect_disconnect);

        /* set functionality first, then enable */
        if(data->android_extra_sysfs_value)
            ret = write_to_file(data->android_extra_sysfs_path, data->android_extra_sysfs_value);

        /* ??? */
        write_to_file(data->android_extra_sysfs_path2, data->android_extra_sysfs_value2);

        /* only works for android since the idProduct is a module parameter */
        android_set_productid(data->idProduct);

        /* only works for android since the idProduct is a module parameter */
        android_set_vendorid(data->idVendorOverride);

        write_to_file(data->sysfs_path, data->sysfs_value);

        /* enable the device */
        if( ret == 0 )
            ret = write_to_file(data->softconnect_path, data->softconnect);
    }
    else if( modules_in_use() ) {
        /* Assume relevant module has already been successfully loaded
         * from somewhere else.
         */
        ret = 0;
    }
    else {
        log_crit("no backend is selected, can't set dynamic mode");
        ret = 1;
    }

    /* functionality should be enabled, so we can enable the network now */
    if(data->network)
    {
#ifdef DEBIAN
        char command[256];

        g_snprintf(command, 256, "ifdown %s ; ifup %s", data->network_interface, data->network_interface);
        usbmoded_system(command);
#else
        network_down(data);
        network = network_up(data);
#endif /* DEBIAN */
    }

    /* try a second time to bring up the network if it failed the first time,
     this can happen with functionfs based gadgets (which is why we sleep for a bit */
    if(network != 0 && data->network)
    {
        log_debug("Retry setting up the network later\n");
        if(delayed_network)
            g_source_remove(delayed_network);
        delayed_network = g_timeout_add_seconds(3, modesetting_network_retry, data);
    }

    /* Needs to be called before application post synching so
     that the dhcp server has the right config */
    if(data->nat || data->dhcp_server)
        network_set_up_dhcpd(data);

    /* no need to execute the post sync if there was an error setting the mode */
    if(data->appsync && !ret)
    {
        /* let's sleep for a bit (350ms) to allow interfaces to settle before running postsync */
        usbmoded_msleep(350);
        appsync_activate_sync_post(data->mode_name);
    }

#ifdef CONNMAN
    if(data->connman_tethering)
        connman_set_tethering(data->connman_tethering, TRUE);
#endif

    if(ret)
        umdbus_send_error_signal(MODE_SETTING_FAILED);
    return(ret);
}

static void modesetting_unset_dynamic_mode(void)
{
    struct mode_list_elem *data;

    data = usbmoded_get_usb_mode_data();

    if(delayed_network)
    {
        g_source_remove(delayed_network);
        delayed_network = 0;
    }

    /* the modelist could be empty */
    if(!data)
        return;

    if(!strcmp(data->mode_name, MODE_MASS_STORAGE))
    {
        modesetting_unset_mass_storage_mode(data);
        return;
    }

#ifdef CONNMAN
    if(data->connman_tethering)
        connman_set_tethering(data->connman_tethering, FALSE);
#endif

    if(data->network)
    {
        network_down(data);
    }

    if( configfs_in_use() ) {
        /* Leave as is. We will reprogram wnen mode is
         * set, not when it is unset.
         */
    }
    else if( android_in_use() ) {
        /* disconnect before changing functionality */
        write_to_file(data->softconnect_path, data->softconnect_disconnect);
        write_to_file(data->sysfs_path, data->sysfs_reset_value);

        /* restore vendorid if the mode had an override */
        if(data->idVendorOverride)
        {
            char *id = config_get_android_vendor_id();
            android_set_vendorid(id);
            g_free(id);
        }

        /* enable after the changes have been made */
        write_to_file(data->softconnect_path, data->softconnect);
    }
    else if( modules_in_use() ) {
        /* Assume unloading happens somewhere else */
    }
    else {
        log_crit("no backend is selected, can't unset dynamic mode");
    }
}

/** clean up mode changes or extra actions to perform after a mode change
 * @param module Name of module currently in use
 * @return 0 on success, non-zero on failure
 *
 */
int modesetting_cleanup(const char *module)
{

    log_debug("Cleaning up mode\n");

    if(!module)
    {
        log_warning("No module found to unload. Skipping cleanup\n");
        return 0;
    }

#ifdef APP_SYNC
    /* Stop applications started due to entering this mode */
    appsync_stop(FALSE);
#endif /* APP_SYNC */

    if(!strcmp(module, MODULE_MASS_STORAGE)|| !strcmp(module, MODULE_FILE_STORAGE))
    {
        /* no clean-up needs to be done when we come from charging mode. We need
         to check since we use fake mass-storage for charging */
        if(!strcmp(MODE_CHARGING, usbmoded_get_usb_mode()) || !strcmp(MODE_CHARGING_FALLBACK, usbmoded_get_usb_mode()))
            return 0;
        modesetting_unset_mass_storage_mode(NULL);
    }

    else if(usbmoded_get_usb_mode_data())
        modesetting_unset_dynamic_mode();

    return(0);
}

/** Allocate modesetting related dynamic resouces
 */
void modesetting_init(void)
{
    if( !tracked_values ) {
        tracked_values = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    }
}

/** Release modesetting related dynamic resouces
 */
void modesetting_quit(void)
{
    if( tracked_values ) {
        g_hash_table_unref(tracked_values), tracked_values = 0;
    }
}
