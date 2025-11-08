/* bjb-local-provider.c
 *
 * Copyright (C) Pierre-Yves LUYTEN 2013 <py@luyten.fr>
 * Copyright 2023 Mohammed Sadiq <sadiq@sadiqpk.org>
 * Copyright 2023 Purism SPC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "bjb-local-provider"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib/gi18n.h>

#include "../items/bjb-xml-note.h"
#include "bjb-local-provider.h"

struct _BjbLocalProvider
{
  BjbProvider  parent_instance;

  char        *domain;
  char        *user_name;

  char        *location;
  char        *trash_location;

  GListStore  *notes;
  GListStore  *trash_notes;
};

G_DEFINE_TYPE (BjbLocalProvider, bjb_local_provider, BJB_TYPE_PROVIDER)

static void
bjb_local_provider_finalize (GObject *object)
{
  BjbLocalProvider *self = (BjbLocalProvider *)object;

  g_clear_pointer (&self->domain, g_free);
  g_clear_pointer (&self->user_name, g_free);
  g_clear_pointer (&self->location, g_free);
  g_clear_pointer (&self->trash_location, g_free);

  g_clear_object (&self->notes);
  g_clear_object (&self->trash_notes);

  G_OBJECT_CLASS (bjb_local_provider_parent_class)->finalize (object);
}

static const char *
bjb_local_provider_get_name (BjbProvider *provider)
{
  return _("Local");
}

static GIcon *
bjb_local_provider_get_icon (BjbProvider  *provider,
                             GError      **error)
{
  return g_icon_new_for_string ("user-home", error);
}

static const char *
bjb_local_provider_get_location_name (BjbProvider *provider)
{
  return _("On This Computer");
}

static DexFuture *
bjb_local_provider_load (BjbLocalProvider *self,
                         const char       *path,
                         GListStore       *store)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  g_autoptr(GFile) location = NULL;
  g_autoptr(GError) error = NULL;
  GList *file_info = NULL;
  DexFuture *future;

  g_assert (BJB_IS_LOCAL_PROVIDER (self));
  g_assert (G_IS_LIST_STORE (store));
  g_assert (path != NULL);

  location = g_file_new_for_path (path);
  future = dex_file_enumerate_children (location,
                                        G_FILE_ATTRIBUTE_STANDARD_NAME","
                                        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                                        G_FILE_QUERY_INFO_NONE,
                                        G_PRIORITY_DEFAULT);
  enumerator = dex_await_object (future, &error);
  if (error)
    return dex_future_new_for_error (g_steal_pointer (&error));

  do
    {
      g_autolist(GFileInfo) infos = NULL;

      future = dex_file_enumerator_next_files (enumerator, 10, G_PRIORITY_DEFAULT);
      infos = file_info = dex_await_boxed (future, &error);

      if (error)
        return dex_future_new_for_error (g_steal_pointer (&error));

      for (GList *info = infos; info && info->data; info = info->next)
        {
          g_autoptr(BjbItem) note = NULL;
          g_autoptr(GFile) file = NULL;
          const char *name;

          name = g_file_info_get_name (info->data);

          if (!g_str_has_suffix (name, ".note"))
            continue;

          file = g_file_get_child (location, name);
          note = bjb_xml_note_new_from_data (g_file_peek_path (file),
                                             bjb_provider_get_tag_store (BJB_PROVIDER (self)));
          g_object_set_data (G_OBJECT (note), "provider", self);
          bjb_item_unset_modified (note);

          if (store == self->trash_notes)
            bjb_item_set_is_trashed (note, TRUE);

          g_list_store_append (store, note);
        }
    } while (file_info);

  return dex_future_new_true ();
}

static DexFuture *
bjb_local_provider_connect (BjbProvider *provider)
{
  BjbLocalProvider *self = (BjbLocalProvider *)provider;

  g_assert (BJB_IS_LOCAL_PROVIDER (self));

  return bjb_local_provider_load (self, self->location, self->notes);
}

static DexFuture *
bjb_local_provider_save_item (BjbProvider *provider,
                              BjbItem     *item)
{
  BjbLocalProvider *self = (BjbLocalProvider *)provider;

  g_assert (BJB_IS_LOCAL_PROVIDER (self));

  if (bjb_item_get_uid (item) == NULL)
    {
      g_autofree char *name = NULL;
      g_autofree char *path = NULL;
      g_autofree char *uuid = NULL;

      uuid = g_uuid_string_random ();
      name = g_strdup_printf ("%s.note", uuid);
      path = g_build_filename (g_get_user_data_dir (), "bijiben", name, NULL);

      bjb_item_set_uid (item, path);
    }

  if (BJB_IS_NOTE (item))
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GBytes) bytes = NULL;
      g_autoptr(GFile) file = NULL;
      char *content;

      content = bjb_note_get_raw_content (BJB_NOTE (item));
      bytes = g_bytes_new_take (content, strlen (content));
      file = g_file_new_for_path (bjb_item_get_uid (item));

      return dex_file_replace_contents_bytes (file, bytes, NULL, FALSE, G_FILE_CREATE_NONE);
    }

  return dex_future_new_true ();
}

static GListModel *
bjb_local_provider_get_notes (BjbProvider *provider)
{
  BjbLocalProvider *self = (BjbLocalProvider *)provider;

  g_assert (BJB_IS_LOCAL_PROVIDER (self));

  return G_LIST_MODEL (self->notes);
}

static GListModel *
bjb_local_provider_get_trash_notes (BjbProvider *provider)
{
  BjbLocalProvider *self = (BjbLocalProvider *)provider;

  g_assert (BJB_IS_LOCAL_PROVIDER (self));

  return G_LIST_MODEL (self->trash_notes);
}

static void
bjb_local_provider_class_init (BjbLocalProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BjbProviderClass *provider_class = BJB_PROVIDER_CLASS (klass);

  object_class->finalize = bjb_local_provider_finalize;

  provider_class->get_name = bjb_local_provider_get_name;
  provider_class->get_icon = bjb_local_provider_get_icon;
  provider_class->get_location_name = bjb_local_provider_get_location_name;
  provider_class->get_notes = bjb_local_provider_get_notes;
  provider_class->get_trash_notes = bjb_local_provider_get_trash_notes;

  provider_class->connect = bjb_local_provider_connect;
  provider_class->save_item = bjb_local_provider_save_item;
}

static void
bjb_local_provider_init (BjbLocalProvider *self)
{
  self->notes = g_list_store_new (BJB_TYPE_ITEM);
  self->trash_notes = g_list_store_new (BJB_TYPE_ITEM);
}

BjbProvider *
bjb_local_provider_new (const char *path)
{
  BjbLocalProvider *self;

  self = g_object_new (BJB_TYPE_LOCAL_PROVIDER, NULL);

  if (path && *path)
    self->location = g_strdup (path);
  else
    self->location = g_build_filename (g_get_user_data_dir (),
                                       "bijiben", NULL);

  self->trash_location = g_build_filename (self->location,
                                           ".Trash", NULL);
  g_mkdir_with_parents (self->location, 0755);
  g_mkdir_with_parents (self->trash_location, 0755);

  return BJB_PROVIDER (self);
}
