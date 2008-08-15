/*
 * This file is part of libhildondesktop
 *
 * Copyright (C) 2006, 2008 Nokia Corporation.
 *
 * Based on hd-desktop.c and hd-plugin-manager.c from hildon-desktop.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>
#include <glib-object.h>
#include <libgnomevfs/gnome-vfs.h>

#include <string.h>

#include "hd-config.h"
#include "hd-plugin-configuration.h"
#include "hd-plugin-loader.h"
#include "hd-plugin-loader-factory.h"
#include "hd-ui-policy.h"

#include "hd-plugin-manager.h"

#define HD_PLUGIN_MANAGER_GET_PRIVATE(object) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((object), HD_TYPE_PLUGIN_MANAGER, HDPluginManagerPrivate))

#define HD_PLUGIN_MANAGER_CONFIG_GROUP "X-PluginManager"
#define HD_PLUGIN_MANAGER_CONFIG_KEY_DEBUG_PLUGINS "X-Debug-Plugins"
#define HD_PLUGIN_MANAGER_CONFIG_KEY_LOAD_ALL_PLUGINS "X-Load-All-Plugins"
#define HD_PLUGIN_MANAGER_CONFIG_KEY_PLUGIN_CONFIGURATION "X-Plugin-Configuration"

enum
{
  PROP_0,
  PROP_CONF_FILE,
  PROP_SAFE_MODE_FILE,
};

enum
{
  PLUGIN_MODULE_ADDED,
  PLUGIN_MODULE_REMOVED,
  PLUGIN_ADDED,
  PLUGIN_REMOVED,
  CONFIGURATION_LOADED,
  LAST_SIGNAL
};

struct _HDPluginManagerPrivate 
{
  GObject                *factory;

  GList                  *plugins;

  HDConfigFile           *config_file;
  gchar                  *safe_mode_file;

  HDPluginConfiguration  *plugin_configuration;

  gboolean                safe_mode;

  gchar                 **plugin_dirs;
  GnomeVFSMonitorHandle **plugin_dir_monitors;

  gboolean                load_new_plugins;
  gboolean                load_all_plugins;

  HDUIPolicy             *policy;

  gchar                 **debug_plugins;
};

static guint plugin_manager_signals [LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (HDPluginManager, hd_plugin_manager, G_TYPE_OBJECT);

static void
delete_plugin (gpointer  data,
               GObject  *object_pointer)
{
  HDPluginInfo *info = ((GList *) data)->data;

  hd_plugin_info_free (info);
  ((GList *) data)->data = NULL;
}

static void
hd_plugin_manager_remove_plugin_module (HDPluginManager *manager,
                                        const gchar     *desktop_file)
{
  HDPluginManagerPrivate *priv = manager->priv;
  GList *p;

  /* remove all plugins with desktop_file */
  for (p = priv->plugins; p; p = p->next)
    {
      HDPluginInfo *info = p->data;

      if (!info)
        continue;

      if (!strcmp (info->desktop_file, desktop_file))
        {
          g_object_weak_unref (G_OBJECT (info->item), delete_plugin, p);
          priv->plugins = g_list_delete_link (priv->plugins, p);
          g_signal_emit (manager, plugin_manager_signals[PLUGIN_REMOVED], 0, info->item);
        }
    }
}

static void
hd_plugin_manager_remove_plugin (HDPluginManager *manager,
                                 const gchar     *plugin_id)
{
  HDPluginManagerPrivate *priv = manager->priv;
  GList *p;

  /* Remove the plugin with id plugin_id*/
  for (p = priv->plugins; p; p = p->next)
    {
      HDPluginInfo *info = p->data;

      if (!info)
        continue;

      if (!strcmp (info->plugin_id, plugin_id))
        {
          g_object_weak_unref (G_OBJECT (info->item), delete_plugin, p);
          priv->plugins = g_list_delete_link (priv->plugins, p);
          g_signal_emit (manager, plugin_manager_signals[PLUGIN_REMOVED], 0, info->item);
          /* There is only one such plugin */
          return;
        }
    }
}

static void
hd_plugin_manager_plugin_dir_changed (GnomeVFSMonitorHandle *handle,
                                      const gchar *monitor_uri,
                                      const gchar *info_uri,
                                      GnomeVFSMonitorEventType event_type,
                                      HDPluginManager *manager)
{
  /* Ignore the temporary dpkg files */
  if (!g_str_has_suffix (info_uri, ".desktop"))
    return;

  if (event_type == GNOME_VFS_MONITOR_EVENT_CREATED)
    {
      GnomeVFSURI *uri = gnome_vfs_uri_new (info_uri);
      gchar *uri_str;

      uri_str = gnome_vfs_uri_to_string (uri, GNOME_VFS_URI_HIDE_TOPLEVEL_METHOD);

      g_debug ("plugin-added: %s", uri_str);

      /* FIXME: dpkg install involves some renaming */
      hd_plugin_manager_remove_plugin_module (manager, uri_str);

      g_signal_emit (manager, plugin_manager_signals[PLUGIN_MODULE_ADDED], 0, uri_str);
    }
  else if (event_type == GNOME_VFS_MONITOR_EVENT_DELETED)
    {
      GnomeVFSURI *uri = gnome_vfs_uri_new (info_uri);
      gchar *uri_str;

      uri_str = gnome_vfs_uri_to_string (uri, GNOME_VFS_URI_HIDE_TOPLEVEL_METHOD);

      g_debug ("plugin-removed: %s", uri_str);

      g_signal_emit (manager, plugin_manager_signals[PLUGIN_MODULE_REMOVED], 0, uri_str);
    }
}

static gboolean 
hd_plugin_manager_load_plugin (HDPluginManager *manager,
                               const gchar     *desktop_file,
                               const gchar     *plugin_id)
{
  HDPluginManagerPrivate *priv;
  HDPluginInfo *info;
  GList *p;
  HDPluginItem *plugin;
  GError *error = NULL;
  gchar *desktop_file_to_load;

  g_return_val_if_fail (HD_IS_PLUGIN_MANAGER (manager), FALSE);
  g_return_val_if_fail (desktop_file != NULL, FALSE);
  g_return_val_if_fail (plugin_id != NULL, FALSE);

  priv = HD_PLUGIN_MANAGER (manager)->priv;

  if (priv->policy)
    {
      desktop_file_to_load = hd_ui_policy_get_filtered_plugin (priv->policy,
                                                               desktop_file,
                                                               priv->safe_mode);

      if (!desktop_file_to_load)
        return FALSE;
    }
  else
    desktop_file_to_load = g_strdup (desktop_file);

  if (!g_file_test (desktop_file_to_load, G_FILE_TEST_EXISTS))
    {
      g_warning ("Plugin desktop file not found, ignoring plugin");
      g_free (desktop_file_to_load);
      return FALSE;
    }

  plugin = hd_plugin_loader_factory_create (HD_PLUGIN_LOADER_FACTORY (manager->priv->factory), 
                                            plugin_id,
                                            desktop_file_to_load,
                                            &error);
  g_free (desktop_file_to_load);

  if (!plugin)
    {
      g_warning ("Error loading plugin: %s", error->message);
      g_error_free (error);

      if (priv->policy)
        {
          desktop_file_to_load = hd_ui_policy_get_default_plugin (priv->policy,
                                                               desktop_file,
                                                               priv->safe_mode);

          if (desktop_file_to_load && g_file_test (desktop_file_to_load, G_FILE_TEST_EXISTS))
            {
              plugin = hd_plugin_loader_factory_create (HD_PLUGIN_LOADER_FACTORY (manager->priv->factory), 
                                                        plugin_id,
                                                        desktop_file_to_load,
                                                        &error);
            }

          g_free (desktop_file_to_load);

          if (!plugin)
            {
              g_error_free (error);

              plugin = (HDPluginItem *) hd_ui_policy_get_failure_plugin (priv->policy,
                                                        desktop_file,
                                                        priv->safe_mode);
            }
        }
    }

  if (!plugin)
    return FALSE;

  info = hd_plugin_info_new (plugin_id,
                             desktop_file,
                             0);
  info->item = plugin;

  g_debug ("Added plugin to list: %s", info->desktop_file);

  p = g_list_append (NULL, info);
  priv->plugins = g_list_concat (priv->plugins, p);

  g_object_weak_ref (G_OBJECT (plugin), delete_plugin, p);

  g_signal_emit (manager, plugin_manager_signals[PLUGIN_ADDED], 0, plugin);

  return TRUE;
}

static void
hd_plugin_manager_safe_mode_init (HDPluginManager *manager)
{
  const gchar *dev_mode = g_getenv ("SBOX_PRELOAD");
  HDPluginManagerPrivate *priv;

  priv = HD_PLUGIN_MANAGER (manager)->priv;

  priv->safe_mode = FALSE;

  if (!dev_mode && priv->safe_mode_file)
    {
      /* 
       * Check for safe mode. The stamp file is created here and
       * Removed in main after gtk_main by g_object_unref in a call to finalize
       * function of this gobject in case of clean non-crash exit 
       * Added by Karoliina <karoliina.t.salminen@nokia.com> 31.7.2007 
       */
      if (g_file_test (priv->safe_mode_file, G_FILE_TEST_EXISTS)) 
        {
          /* Enters safe mode */
          g_warning ("The program did not exit properly on the previous "
                     "session. All plugins will be disabled.");

          priv->safe_mode = TRUE;
        } 
      else 
        {
          gchar *stamp_dir = g_path_get_dirname (priv->safe_mode_file);

          /* Hildon Desktop enters normal mode and creates the stamp to track crashes */
          g_mkdir_with_parents (stamp_dir, 0755);

          g_file_set_contents (priv->safe_mode_file, "1", 1, NULL);

          priv->safe_mode = FALSE;
        }
    }
}

static void
hd_plugin_manager_init (HDPluginManager *manager)
{
  manager->priv = HD_PLUGIN_MANAGER_GET_PRIVATE (manager);

  manager->priv->factory = hd_plugin_loader_factory_new (); 
}

static void
hd_plugin_manager_plugin_module_added (HDPluginManager *manager,
                                       const gchar     *desktop_file)
{
  HDPluginManagerPrivate *priv;

  g_return_if_fail (HD_IS_PLUGIN_MANAGER (manager));

  priv = HD_PLUGIN_MANAGER (manager)->priv;

  if (priv->load_new_plugins && !priv->safe_mode)
    {
      gchar *plugin_id;

      plugin_id = g_path_get_basename (desktop_file);

      hd_plugin_manager_load_plugin (manager, desktop_file, plugin_id);

      g_free (plugin_id);
    }
  else if (priv->debug_plugins != NULL)
    {
      gchar *plugin_id;
      guint i;

      plugin_id = g_path_get_basename (desktop_file);

      for (i = 0; priv->debug_plugins[i]; i++)
        {
          if (strcmp (plugin_id, priv->debug_plugins[i]))
            hd_plugin_manager_load_plugin (manager, desktop_file, plugin_id);
        }

      g_free (plugin_id);
    }
}

static void
hd_plugin_manager_plugin_module_removed (HDPluginManager *manager,
                                         const gchar     *desktop_file)
{
  hd_plugin_manager_remove_plugin_module (manager, desktop_file);
}

static void
hd_plugin_manager_plugin_added (HDPluginManager *manager,
                                GObject         *plugin)
{
}

static void
hd_plugin_manager_plugin_removed (HDPluginManager *manager,
                                  GObject         *plugin)
{
}

static void
hd_plugin_manager_finalize (GObject *object)
{
  HDPluginManagerPrivate *priv;

  g_return_if_fail (HD_IS_PLUGIN_MANAGER (object));

  priv = HD_PLUGIN_MANAGER (object)->priv;

  if (priv->factory)
    {
      g_object_unref (priv->factory);
      priv->factory = NULL;
    }

  if (priv->config_file)
    {
      g_object_unref (priv->config_file);
      priv->config_file = NULL;
    }

  g_free (priv->safe_mode_file);
  priv->safe_mode_file = NULL;

  if (priv->plugin_dirs != NULL)
    {
      guint i;
      for (i = 0; priv->plugin_dirs[i] != NULL; i++)
        {
          gnome_vfs_monitor_cancel (priv->plugin_dir_monitors[i]);
        }
      g_free (priv->plugin_dir_monitors);
      priv->plugin_dir_monitors = NULL;
      g_strfreev (priv->plugin_dirs);
      priv->plugin_dirs = NULL;
    }

  g_strfreev (priv->debug_plugins);
  priv->debug_plugins = NULL;

  if (priv->policy)
    {
      g_object_unref (priv->policy);
      priv->policy = NULL;
    }

  G_OBJECT_CLASS (hd_plugin_manager_parent_class)->finalize (object);
}

/* Compare lists new and old and move elements unique in old
 * to to_remove and elements unique in new to to_add, elements
 * common in new and old are removed.
 *
 * old is destroyed by this function
 * new is destroyed by this function
 */
static void
create_sync_lists (GList          *old,
                   GList          *new,
                   GList         **to_add,
                   GList         **to_remove,
                   GCompareFunc    cmp_func,
                   GDestroyNotify  destroy_func)
{
  GList *add = NULL;
  GList *remove = NULL;

  g_return_if_fail (to_add != NULL);
  g_return_if_fail (to_remove != NULL);

  /* sort lists */
  old = g_list_sort (old, cmp_func);
  new = g_list_sort (new, cmp_func);

  while (old && new)
    {
      gint c = cmp_func (old->data, new->data);

      /* there is an element only in new 
       * move it to list to_add */
      if (c > 0)
        {
          GList *n = new;
          new = g_list_remove_link (new, new);
          add = g_list_concat (n, add);
        }
      /* there is an element only in old 
       * move it to list to_remove */
      else if (c < 0)
        {
          GList *o = old;
          old = g_list_remove_link (old, old);
          remove = g_list_concat (o, remove);
        }
      /* the next element is in old and new
       * remove it */
      else
        {
          destroy_func (old->data);
          destroy_func (new->data);

          old = g_list_delete_link (old, old);
          new = g_list_delete_link (new, new);
        }
    }

  /* add remaining elements to the approbiate lists */
  *to_add = g_list_concat (new, add);
  *to_remove = g_list_concat (old, remove);
}

static gint
cmp_info_plugin_id (const HDPluginInfo *a,
                    const HDPluginInfo *b)
{
  return strcmp (a->plugin_id, b->plugin_id);
}

static gint
cmp_info_desktop_file (const HDPluginInfo *a,
                       const HDPluginInfo *b)
{
  return strcmp (a->desktop_file, b->desktop_file);
}

static gint
cmp_info_priority (const HDPluginInfo *a,
                   const HDPluginInfo *b)
{
  if (a->priority != b->priority)
    return a->priority - b->priority;

  return strcmp (a->plugin_id, b->plugin_id);
}

/* sync new_plugins with already loaded plugins 
 *
 * new_plugins is destroyed by this function
 */
static void
hd_plugin_manager_sync_plugins (HDPluginManager *manager,
                                GList           *new_plugins)
{
  HDPluginManagerPrivate *priv = manager->priv;
  GList *old_plugins = NULL;
  GList *p;
  GList *to_add = NULL, *to_remove = NULL;

  for (p = priv->plugins; p; p = p->next)
    {
      HDPluginInfo *info = p->data;

      if (!info)
        continue;

      old_plugins = g_list_prepend (old_plugins, hd_plugin_info_new (info->plugin_id,
                                                                           info->desktop_file,
                                                                           0));
    }

  create_sync_lists (old_plugins, new_plugins, &to_add, &to_remove,
                     (GCompareFunc) cmp_info_plugin_id, (GDestroyNotify) hd_plugin_info_free);

  /* remove plugins */
  for (p = to_remove; p; p = p->next)
    {
      HDPluginInfo *info = p->data;

      hd_plugin_manager_remove_plugin (manager, info->plugin_id);

      hd_plugin_info_free (info);
    }

  to_add = g_list_sort (to_add, (GCompareFunc) cmp_info_priority);

  /* add plugins */
  for (p = to_add; p; p = p->next)
    {
      HDPluginInfo *info = p->data;

      hd_plugin_manager_load_plugin (manager, info->desktop_file, info->plugin_id);

      hd_plugin_info_free (info);
    }

  g_list_free (to_remove);
  g_list_free (to_add);
}

static void
hd_plugin_manager_load_configuration (HDPluginManager *manager)
{
  HDPluginManagerPrivate *priv = manager->priv;
  GKeyFile *keyfile;

  /* load new configuration */
  keyfile = hd_config_file_load_file (priv->config_file, FALSE);

  if (!keyfile)
    {
      g_warning ("Error loading configuration file");

      return;
    }

  g_signal_emit (manager, plugin_manager_signals[CONFIGURATION_LOADED], 0, keyfile);

  g_key_file_free (keyfile);
}

static void
hd_plugin_manager_configuration_loaded (HDPluginManager *manager,
                                        GKeyFile        *keyfile)
{
  HDPluginManagerPrivate *priv = manager->priv;
  GError *error = NULL;
  gsize n_plugin_dir;
  gchar *policy_module;
  gchar *plugin_configuration_filename;
  GList *new_plugins = NULL;

  /* free old configuration */
  if (priv->plugin_dirs != NULL)
    {
      guint i;

      for (i = 0; priv->plugin_dirs[i] != NULL; i++)
        {
          gnome_vfs_monitor_cancel (priv->plugin_dir_monitors[i]);
        }
      g_free (priv->plugin_dir_monitors);
      priv->plugin_dir_monitors = NULL;
      g_strfreev (priv->plugin_dirs);
      priv->plugin_dirs = NULL;
    }
  g_strfreev (priv->debug_plugins);
  priv->debug_plugins = NULL;
  if (priv->policy)
    {
      g_object_unref (priv->policy);
      priv->policy = NULL;
    }
  if (priv->plugin_configuration)
    {
      g_object_unref (priv->plugin_configuration);
      priv->plugin_configuration = NULL;      
    }

  /* Load configuration ([X-PluginManager] group) */
  if (!g_key_file_has_group (keyfile, HD_PLUGIN_MANAGER_CONFIG_GROUP))
    {
      g_warning ("Error configuration file doesn't contain group '%s'",
                 HD_PLUGIN_MANAGER_CONFIG_GROUP);

      return;
    }

  priv->load_new_plugins = g_key_file_get_boolean (keyfile, 
                                                   HD_PLUGIN_MANAGER_CONFIG_GROUP, 
                                                   HD_DESKTOP_CONFIG_KEY_LOAD_NEW_PLUGINS,
                                                   NULL);
  priv->load_all_plugins = g_key_file_get_boolean (keyfile, 
                                                   HD_PLUGIN_MANAGER_CONFIG_GROUP, 
                                                   HD_PLUGIN_MANAGER_CONFIG_KEY_LOAD_ALL_PLUGINS,
                                                   NULL);

  priv->plugin_dirs = g_key_file_get_string_list (keyfile,
                                                  HD_PLUGIN_MANAGER_CONFIG_GROUP,
                                                  HD_DESKTOP_CONFIG_KEY_PLUGIN_DIR,
                                                  &n_plugin_dir,
                                                  &error);

  if (!priv->plugin_dirs)
    {
      g_warning ("Error loading configuration file. No plugin dirs defined: %s",
                 error->message);

      g_error_free (error);

      return;
    }
  else 
    {
      guint i;

      priv->plugin_dir_monitors = g_new0 (GnomeVFSMonitorHandle*, n_plugin_dir);

      for (i = 0; priv->plugin_dirs[i] != NULL; i++)
        {
          gnome_vfs_monitor_add (&priv->plugin_dir_monitors[i],
                                 priv->plugin_dirs[i],
                                 GNOME_VFS_MONITOR_DIRECTORY,
                                 (GnomeVFSMonitorCallback) hd_plugin_manager_plugin_dir_changed,
                                 manager);
        }
    }

  priv->debug_plugins = g_key_file_get_string_list (keyfile,
                                                    HD_PLUGIN_MANAGER_CONFIG_GROUP,
                                                    HD_PLUGIN_MANAGER_CONFIG_KEY_DEBUG_PLUGINS,
                                                    NULL,
                                                    &error);

  policy_module = g_key_file_get_string (keyfile, 
                                         HD_PLUGIN_MANAGER_CONFIG_GROUP, 
                                         HD_DESKTOP_CONFIG_KEY_UI_POLICY,
                                         &error);

  if (error)
    {
      g_free (policy_module);

      g_error_free (error);
      error = NULL;
    }
  else 
    {
      gchar *policy_module_path = g_build_filename (HD_UI_POLICY_MODULES_PATH,
                                                    policy_module,
                                                    NULL);

      if (g_file_test (policy_module_path, G_FILE_TEST_EXISTS))
        {
          priv->policy = hd_ui_policy_new (policy_module_path);
        }
      else
        {
          g_warning ("Container's UI policy module doesn't exist. Not applying policy then.");
        }

      g_free (policy_module_path);
    }

  plugin_configuration_filename = g_key_file_get_string (keyfile, 
                                                         HD_PLUGIN_MANAGER_CONFIG_GROUP, 
                                                         HD_PLUGIN_MANAGER_CONFIG_KEY_PLUGIN_CONFIGURATION,
                                                         NULL);

  if (plugin_configuration_filename)
    {
      priv->plugin_configuration = hd_plugin_configuration_new_for_config_file (priv->config_file,
                                                                                plugin_configuration_filename);

      new_plugins = hd_plugin_configuration_get_plugins (priv->plugin_configuration, FALSE);
    }

  /* Load all plugins in the X-Plugin-Dirs directories 
   * if X-Load-All-Plugins is true */
  if (priv->load_all_plugins)
    {
      GList *all_plugins, *p;

      all_plugins = hd_plugin_manager_get_all_plugin_paths (manager);

      for (p = all_plugins; p; p = p->next)
        {
          HDPluginInfo *info = hd_plugin_info_new (NULL,
                                                   p->data,
                                                   G_MAXUINT);

          if (!g_list_find_custom (new_plugins, info, (GCompareFunc) cmp_info_desktop_file))
            {
              info->plugin_id = g_path_get_basename (info->desktop_file);
              new_plugins = g_list_prepend (new_plugins, info);
            }
          else
            hd_plugin_info_free (info);
        }

      g_list_foreach (all_plugins, (GFunc) g_free, NULL);
      g_list_free (all_plugins);
    }

  /* Don't load plugins from X-Debug-Plugins list */
  if (priv->debug_plugins != NULL)
    {
      guint i;

      for (i = 0; priv->debug_plugins[i]; i++)
        {
          GList *p;

          for (p = new_plugins; p; )
            {
              HDPluginInfo *info = p->data;
              gchar *basename = g_path_get_basename (info->desktop_file);

              if (!strcmp (basename, priv->debug_plugins[i]))
                {
                  GList *q = p->next;

                  hd_plugin_info_free (info);
                  new_plugins = g_list_delete_link (new_plugins, p);

                  p = q;
                }
              else
                p = p->next;

              g_free (basename);
            }
        }
    }

  hd_plugin_manager_sync_plugins (manager, new_plugins);
}

static void
hd_plugin_manager_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  HDPluginManagerPrivate *priv= HD_PLUGIN_MANAGER (object)->priv;

  switch (prop_id)
    {
    case PROP_CONF_FILE:
      priv->config_file = g_value_dup_object (value);
      if (priv->config_file != NULL)
        g_signal_connect_object (priv->config_file, "changed",
                                 G_CALLBACK (hd_plugin_manager_load_configuration),
                                 object, G_CONNECT_SWAPPED);
      break;

    case PROP_SAFE_MODE_FILE:
      g_free (priv->safe_mode_file);
      priv->safe_mode_file = g_value_dup_string (value);
      break;


    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
hd_plugin_manager_get_property (GObject      *object,
                                guint         prop_id,
                                GValue       *value,
                                GParamSpec   *pspec)
{
  HDPluginManagerPrivate *priv = HD_PLUGIN_MANAGER (object)->priv;

  switch (prop_id)
    {
    case PROP_SAFE_MODE_FILE:
      g_value_set_string (value, priv->safe_mode_file);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
hd_plugin_manager_class_init (HDPluginManagerClass *klass)
{
  GObjectClass *g_object_class = (GObjectClass *) klass;

  klass->plugin_module_added = hd_plugin_manager_plugin_module_added;
  klass->plugin_module_removed = hd_plugin_manager_plugin_module_removed;
  klass->plugin_added = hd_plugin_manager_plugin_added;
  klass->plugin_removed = hd_plugin_manager_plugin_removed;
  klass->configuration_loaded = hd_plugin_manager_configuration_loaded;

  g_object_class->finalize = hd_plugin_manager_finalize;
  g_object_class->get_property = hd_plugin_manager_get_property;
  g_object_class->set_property = hd_plugin_manager_set_property;

  g_type_class_add_private (g_object_class, sizeof (HDPluginManagerPrivate));

  g_object_class_install_property (g_object_class,
                                   PROP_CONF_FILE,
                                   g_param_spec_object ("conf-file",
                                                        "conf-file",
                                                        "Configuration file",
                                                        HD_TYPE_CONFIG_FILE,
                                                        G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (g_object_class,
                                   PROP_SAFE_MODE_FILE,
                                   g_param_spec_string ("safe-mode-file",
                                                        "safe-mode-file",
                                                        "Safe mode stamp file",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  plugin_manager_signals [PLUGIN_MODULE_ADDED] = g_signal_new ("plugin-module-added",
                                                               G_TYPE_FROM_CLASS (klass),
                                                               G_SIGNAL_RUN_FIRST,
                                                               G_STRUCT_OFFSET (HDPluginManagerClass, plugin_module_added),
                                                               NULL, NULL,
                                                               g_cclosure_marshal_VOID__STRING,
                                                               G_TYPE_NONE, 1,
                                                               G_TYPE_STRING);

  plugin_manager_signals [PLUGIN_MODULE_REMOVED] = g_signal_new ("plugin-module-removed",
                                                                 G_TYPE_FROM_CLASS (klass),
                                                                 G_SIGNAL_RUN_FIRST,
                                                                 G_STRUCT_OFFSET (HDPluginManagerClass, plugin_module_removed),
                                                                 NULL, NULL,
                                                                 g_cclosure_marshal_VOID__STRING,
                                                                 G_TYPE_NONE, 1,
                                                                 G_TYPE_STRING);

  plugin_manager_signals [PLUGIN_ADDED] = g_signal_new ("plugin-added",
                                                        G_TYPE_FROM_CLASS (klass),
                                                        G_SIGNAL_RUN_FIRST,
                                                        G_STRUCT_OFFSET (HDPluginManagerClass, plugin_added),
                                                        NULL, NULL,
                                                        g_cclosure_marshal_VOID__OBJECT,
                                                        G_TYPE_NONE, 1,
                                                        G_TYPE_OBJECT);

  plugin_manager_signals [PLUGIN_REMOVED] = g_signal_new ("plugin-removed",
                                                          G_TYPE_FROM_CLASS (klass),
                                                          G_SIGNAL_RUN_LAST,
                                                          G_STRUCT_OFFSET (HDPluginManagerClass,
                                                                           plugin_removed),
                                                          NULL, NULL,
                                                          g_cclosure_marshal_VOID__OBJECT,
                                                          G_TYPE_NONE, 1,
                                                          G_TYPE_OBJECT);

  plugin_manager_signals [CONFIGURATION_LOADED] = g_signal_new ("configuration-loaded",
                                                                G_TYPE_FROM_CLASS (klass),
                                                                G_SIGNAL_RUN_LAST,
                                                                G_STRUCT_OFFSET (HDPluginManagerClass,
                                                                                 configuration_loaded),
                                                                NULL, NULL,
                                                                g_cclosure_marshal_VOID__POINTER,
                                                                G_TYPE_NONE, 1,
                                                                G_TYPE_POINTER);
}

HDPluginManager *
hd_plugin_manager_new (HDConfigFile *config_file,
                       const gchar  *safe_mode_file)
{
  HDPluginManager *manager = g_object_new (HD_TYPE_PLUGIN_MANAGER,
                                           "conf-file", config_file,
                                           "safe-mode-file", safe_mode_file,
                                           NULL);

  return manager;
}

void
hd_plugin_manager_run (HDPluginManager *manager)
{
  HDPluginManagerPrivate *priv;

  g_return_if_fail (HD_IS_PLUGIN_MANAGER (manager));
 
  priv = manager->priv;

  hd_plugin_manager_safe_mode_init (manager);

  hd_plugin_manager_load_configuration (manager);
}

GList *
hd_plugin_manager_get_all_plugin_paths (HDPluginManager *manager)
{
  HDPluginManagerPrivate *priv;
  guint i;
  GList *plugin_paths = NULL;

  priv = HD_PLUGIN_MANAGER_GET_PRIVATE (manager);

  for (i = 0; priv->plugin_dirs[i] != NULL; i++)
    {
      GDir *dir;
      GError *error = NULL;
      const gchar *name;

      dir = g_dir_open (priv->plugin_dirs[i], 0, &error);

      if (dir == NULL)
        {
          g_warning ("Couldn't read plugin_paths in dir %s. Error: %s", priv->plugin_dirs[i], error->message);
          g_error_free (error);
          continue;
        }

      for (name = g_dir_read_name (dir); name != NULL; name = g_dir_read_name (dir))
        {
          gchar *filename;

          filename = g_build_filename (priv->plugin_dirs[i], name, NULL);

          plugin_paths = g_list_append (plugin_paths, filename);
        }

      g_dir_close (dir);
    }

  return plugin_paths;
}


