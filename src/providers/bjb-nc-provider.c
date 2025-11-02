/* bjb-nc-provider.c
 *
 * Copyright 2023 Mohammed Sadiq <sadiq@sadiqpk.org>
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

#define G_LOG_DOMAIN "bjb-nc-provider"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

#include "../items/bjb-item.h"
#include "../items/bjb-plain-note.h"
#include "bjb-dex.h"
#include "bjb-nc-provider.h"
#include "../bjb-log.h"

#define MAX_CONNECTIONS      5
#define DATA_BLOCK_SIZE      8192
#define NEXTCLOUD_BASE_PATH  "/index.php/apps/notes/api/v1"

struct _BjbNcProvider
{
  BjbProvider  parent_instance;

  char        *uid;
  char        *name;
  char        *icon_name;

  char        *domain_name;
  char        *user_name;
  char        *password;
  char        *location_name;
  GUri        *nextcloud_uri;

  GoaObject   *goa_object;
  SoupSession *soup_session;

  GListStore  *notes;
};

G_DEFINE_TYPE (BjbNcProvider, bjb_nc_provider, BJB_TYPE_PROVIDER)

static SoupMessage *
nc_provider_new_message (BjbNcProvider *self,
                         const char    *method,
                         GUri          *uri)
{
  SoupMessage *message;
  SoupSessionFeature *auth_feature;
  g_autoptr(SoupAuth) auth = NULL;

  auth_feature = soup_session_get_feature (self->soup_session, SOUP_TYPE_AUTH_MANAGER);
  message = soup_message_new_from_uri (method, uri);
  auth = soup_auth_new (SOUP_TYPE_AUTH_BASIC, message, "Basic");
  soup_auth_manager_use_auth (SOUP_AUTH_MANAGER (auth_feature), uri, auth);

  return message;
}

static GBytes *
nc_provider_get_note_json (BjbNote *note)
{
  g_autoptr(JsonObject) json = NULL;
  g_autoptr(JsonNode) node = NULL;
  BjbItem *notebook;
  const char *notebook_title;
  char *json_str;

  g_assert (BJB_IS_NOTE (note));

  notebook = bjb_note_get_notebook (note);
  notebook_title = bjb_item_get_title (notebook);

  /* https://github.com/nextcloud/notes/blob/master/docs/api/v1.md */
  json = json_object_new ();
  json_object_set_string_member (json, "title", bjb_item_get_title (BJB_ITEM (note)));
  json_object_set_string_member (json, "content", bjb_note_get_text_content (note));
  json_object_set_string_member (json, "category", notebook_title);
  json_object_set_int_member (json, "modified", time (NULL));

  node = json_node_new (JSON_NODE_OBJECT);
  json_node_init_object (node, json);

  json_str = json_to_string (node, FALSE);
  return g_bytes_new_take (json_str, strlen (json_str));
}

static void
bjb_nc_provider_finalize (GObject *object)
{
  BjbNcProvider *self = (BjbNcProvider *)object;

  g_clear_object (&self->goa_object);

  g_clear_pointer (&self->uid, g_free);
  g_clear_pointer (&self->name, g_free);
  g_clear_pointer (&self->icon_name, g_free);
  g_clear_pointer (&self->domain_name, g_free);
  g_clear_pointer (&self->location_name, g_free);
  g_clear_pointer (&self->nextcloud_uri, g_uri_unref);

  G_OBJECT_CLASS (bjb_nc_provider_parent_class)->finalize (object);
}

static const char *
bjb_nc_provider_get_name (BjbProvider *provider)
{
  BjbNcProvider *self = BJB_NC_PROVIDER (provider);

  return self->name ? self->name : "";
}

static GIcon *
bjb_nc_provider_get_icon (BjbProvider  *provider,
                          GError      **error)
{
  BjbNcProvider *self = BJB_NC_PROVIDER (provider);

  return g_icon_new_for_string (self->icon_name, error);
}

static const char *
bjb_nc_provider_get_location_name (BjbProvider *provider)
{
  BjbNcProvider *self = BJB_NC_PROVIDER (provider);

  return self->location_name;
}

static GListModel *
bjb_nc_provider_get_notes (BjbProvider *provider)
{
  BjbNcProvider *self = (BjbNcProvider *)provider;

  g_assert (BJB_IS_NC_PROVIDER (self));

  return G_LIST_MODEL (self->notes);
}

static void
parse_json_array_cb (JsonArray *array,
                     guint      index_,
                     JsonNode  *node,
                     gpointer   user_data)
{
  BjbNcProvider *self = user_data;
  g_autoptr(BjbItem) note = NULL;
  g_autofree char *id_text = NULL;
  JsonObject *json;
  const char *title, *content;
  gint64 id, mtime;

  json = json_node_get_object (node);
  id = json_object_get_int_member (json, "id");
  title = json_object_get_string_member (json, "title");
  mtime = json_object_get_int_member (json, "modified");
  content = json_object_get_string_member (json, "content");

  id_text = g_strdup_printf ("%ld", id);
  note = bjb_plain_note_new_from_data (id_text, title, content);
  bjb_item_set_mtime (note, mtime);
  bjb_item_unset_modified (note);
  g_object_set_data (G_OBJECT (note), "provider", self);

  if (json_object_has_member(json, "category")) {
    const char *notebook_title;

    notebook_title = json_object_get_string_member(json, "category");
    if (notebook_title && *notebook_title) {
      BjbTagStore *tag_store;
      BjbItem *notebook;

      tag_store = bjb_provider_get_tag_store(BJB_PROVIDER(self));
      notebook = bjb_tag_store_add_notebook(tag_store, notebook_title);
      bjb_note_set_notebook(BJB_NOTE(note), notebook);
    }
  }

  g_list_store_append (self->notes, note);
}

static DexFuture *
provider_load_fiber (gpointer data)
{
  BjbNcProvider *self = data;
  g_autoptr(SoupMessage) message = NULL;
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GUri) uri = NULL;
  JsonNode *root = NULL;
  GInputStream *stream;
  GError *error = NULL;
  DexFuture *future;
  BjbDex *dex;

  g_assert (BJB_IS_NC_PROVIDER (self));

  self->nextcloud_uri = g_uri_build_with_user (SOUP_HTTP_URI_FLAGS, "https",
                                               self->user_name, self->password,
                                               NULL,
                                               self->domain_name, -1,
                                               NEXTCLOUD_BASE_PATH,
                                               NULL, NULL);
  uri = soup_uri_copy (self->nextcloud_uri, SOUP_URI_PATH, NEXTCLOUD_BASE_PATH "/notes", SOUP_URI_NONE);
  message = nc_provider_new_message (self, SOUP_METHOD_GET, uri);

  dex = bjb_dex_new (self->soup_session, soup_session_send_finish, G_TYPE_OBJECT);
  future = bjb_dex_dup_future (dex);
  soup_session_send_async (self->soup_session, message, G_PRIORITY_DEFAULT,
                           bjb_dex_get_cancellable (dex),
                           bjb_dex_callback, dex);

  stream = dex_await_object (future, &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Error session send: %s", error->message);
      return dex_future_new_for_error (error);
    }

  parser = json_parser_new ();
  dex = bjb_dex_new (parser, json_parser_load_from_stream_finish, G_TYPE_BOOLEAN);
  future = bjb_dex_dup_future (dex);
  json_parser_load_from_stream_async (parser, stream,
                                      bjb_dex_get_cancellable (dex),
                                      bjb_dex_callback, dex);
  dex_await_boolean (future, &error);
  if (error)
    return dex_future_new_for_error (error);

  root = json_parser_get_root (parser);
  if (JSON_NODE_TYPE (root) == JSON_NODE_ARRAY)
    json_array_foreach_element (json_node_get_array (root), parse_json_array_cb, self);

  return dex_future_new_true ();
}

static DexFuture *
bjb_nc_provider_connect (BjbProvider *provider)
{
  BjbNcProvider *self = (BjbNcProvider *)provider;
  g_autoptr(GAsyncResult) result = NULL;
  GoaPasswordBased *goa_password;
  DexFuture *future = NULL;
  GError *error = NULL;
  BjbDex *dex;

  g_assert (BJB_IS_NC_PROVIDER (self));

  goa_password = goa_object_peek_password_based (self->goa_object);
  if (!goa_password)
    return dex_future_new_reject (G_IO_ERROR,
                                  G_IO_ERROR_FAILED,
                                  "Failed to connect: Password not available");

  dex = bjb_dex_new (NULL, NULL, G_TYPE_ASYNC_RESULT);
  future = bjb_dex_dup_future (dex);
  goa_password_based_call_get_password (goa_password, self->user_name,
                                        bjb_dex_get_cancellable (dex),
                                        bjb_dex_callback, dex);
  result = dex_await_object (future, &error);
  goa_password_based_call_get_password_finish (goa_password, &self->password, result, &error);

  if (error)
    return dex_future_new_for_error (error);

  dex_future_disown (dex_scheduler_spawn (NULL, 0,
                                          provider_load_fiber,
                                          g_object_ref (self),
                                          g_object_unref));
  return dex_future_new_true ();
}

static DexFuture *
bjb_nc_provider_save_item (BjbProvider *provider,
                           BjbItem     *item)
{
  BjbNcProvider *self = (BjbNcProvider *)provider;

  g_assert (BJB_IS_NC_PROVIDER (self));

  if (self->password)
    {
      g_autoptr(SoupMessage) message = NULL;
      g_autoptr(GBytes) bytes = NULL;
      g_autofree char *path = NULL;
      g_autoptr(GUri) uri = NULL;
      GError *error = NULL;
      DexFuture *future;
      BjbDex *dex;

      if (bjb_item_get_uid (item))
        path = g_strconcat (NEXTCLOUD_BASE_PATH, "/notes/", bjb_item_get_uid (item), NULL);
      else
        path = g_strconcat (NEXTCLOUD_BASE_PATH, "/", "notes", NULL);

      uri = soup_uri_copy (self->nextcloud_uri, SOUP_URI_PATH, path, SOUP_URI_NONE);

      if (bjb_item_get_uid (item))
        message = nc_provider_new_message (self, SOUP_METHOD_PUT, uri);
      else
        message = nc_provider_new_message (self, SOUP_METHOD_POST, uri);

      bytes = nc_provider_get_note_json (BJB_NOTE (item));
      soup_message_set_request_body_from_bytes (message, "application/json", bytes);

      dex = bjb_dex_new (self->soup_session, soup_session_send_finish, G_TYPE_OBJECT);
      future = bjb_dex_dup_future (dex);
      soup_session_send_async (self->soup_session, message, G_PRIORITY_DEFAULT, NULL,
                               bjb_dex_callback, dex);
      dex_await (future, &error);
      if (error)
        return dex_future_new_for_error (error);
      return dex_future_new_true ();
    }
  else
    {
      return dex_future_new_reject (G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Failed to connect: Password not available");
    }
}


static void
bjb_nc_provider_class_init (BjbNcProviderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  BjbProviderClass *provider_class = BJB_PROVIDER_CLASS (klass);

  object_class->finalize = bjb_nc_provider_finalize;

  provider_class->get_name = bjb_nc_provider_get_name;
  provider_class->get_icon = bjb_nc_provider_get_icon;
  provider_class->get_location_name = bjb_nc_provider_get_location_name;
  provider_class->get_notes = bjb_nc_provider_get_notes;

  provider_class->connect = bjb_nc_provider_connect;
  provider_class->save_item = bjb_nc_provider_save_item;
}

static void
bjb_nc_provider_init (BjbNcProvider *self)
{
  self->notes = g_list_store_new (BJB_TYPE_ITEM);
  self->soup_session = g_object_new (SOUP_TYPE_SESSION,
                                     "max-conns-per-host", MAX_CONNECTIONS,
                                     NULL);
}

BjbProvider *
bjb_nc_provider_new (GoaObject *object)
{
  BjbNcProvider *self;
  GoaAccount *account;
  const char *goa_id;

  g_return_val_if_fail (GOA_IS_OBJECT (object), NULL);

  account = goa_object_peek_account (object);
  goa_id = goa_account_get_presentation_identity (account);

  if (!goa_id || !strchr (goa_id, '@'))
    return NULL;

  self = g_object_new (BJB_TYPE_NC_PROVIDER, NULL);
  self->user_name = goa_account_dup_identity (account);
  /* Get the component after last '@' */
  self->domain_name = g_strdup (strrchr (goa_id, '@') + 1);
  self->location_name = g_strdup_printf ("%s@%s", self->user_name, self->domain_name);

  self->goa_object = g_object_ref (object);
  self->uid = goa_account_dup_id (account);
  self->name = goa_account_dup_provider_name (account);
  self->icon_name = goa_account_dup_provider_icon (account);

  return BJB_PROVIDER (self);
}

gboolean
bjb_nc_provider_matches_goa (BjbNcProvider *self,
                             GoaObject     *object)
{
  g_return_val_if_fail (BJB_IS_NC_PROVIDER (self), FALSE);
  g_return_val_if_fail (GOA_IS_OBJECT (object), FALSE);

  if (self->goa_object == object)
    return TRUE;

  return FALSE;
}
