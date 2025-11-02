/* bjb-manager.c
 *
 * Copyright 2012 Pierre-Yves LUYTEN <py@luyten.fr>
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

#define G_LOG_DOMAIN "bjb-manager"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#include <libedataserver/libedataserver.h>
#include <libecal/libecal.h>

#include "items/bjb-plain-note.h"
#include "providers/bjb-provider.h"
#include "providers/bjb-local-provider.h"
#include "providers/bjb-memo-provider.h"
#include "providers/bjb-nc-provider.h"
#include "bjb-dex.h"
#include "bjb-manager.h"

struct _BjbManager
{
  GObject              parent_instance;

  ESourceRegistry     *eds_registry;
  GoaClient           *goa_client;
  BjbProvider         *local_provider;
  GListStore          *providers;

  GListStore          *list_of_notes;
  GtkFlattenListModel *notes;

  gboolean             is_loading;
  gboolean             loaded;
};


G_DEFINE_TYPE (BjbManager, bjb_manager, G_TYPE_OBJECT)

enum {
  ITEM_REMOVED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
manager_provider_item_removed_cb (BjbManager  *self,
                                  BjbItem     *item,
                                  BjbProvider *provider)
{
  g_assert (BJB_IS_MANAGER (self));
  g_assert (BJB_IS_ITEM (item));
  g_assert (BJB_IS_PROVIDER (provider));

  g_signal_emit (self, signals[ITEM_REMOVED], 0, provider, item);
}

static DexFuture *
bjb_manager_load_eds (BjbManager *self)
{
  g_autoptr(GPtrArray) futures = NULL;
  g_autolist(ESource) sources = NULL;
  g_autoptr(GError) error = NULL;

  sources = e_source_registry_list_sources (self->eds_registry,
                                            E_SOURCE_EXTENSION_MEMO_LIST);
  futures = g_ptr_array_new_full (10, dex_unref);
  for (GList *node = sources; node != NULL; node = node->next)
    {
      g_autoptr(BjbProvider) provider = NULL;

      provider = bjb_memo_provider_new (node->data);
      g_signal_connect_object (provider, "item-removed",
                               G_CALLBACK (manager_provider_item_removed_cb),
                               self, G_CONNECT_SWAPPED);
      g_list_store_append (self->providers, provider);
      g_list_store_append (self->list_of_notes,
                           bjb_provider_get_notes (provider));

      g_ptr_array_add (futures, bjb_provider_connect (provider));
    }

  if (futures->len)
    return dex_future_allv ((DexFuture **)futures->pdata, futures->len);

  return dex_future_new_true ();
}

static void
manager_goa_account_added_cb (BjbManager *self,
                              GoaObject  *object,
                              GoaClient  *client)
{
  GoaAccount *account;
  const char *type;

  g_assert (BJB_IS_MANAGER (self));
  g_assert (GOA_IS_CLIENT (client));
  g_assert (GOA_IS_OBJECT (object));

  account = goa_object_peek_account (object);
  type = goa_account_get_provider_type (account);

  if (g_strcmp0 (type, "owncloud") == 0 &&
      !goa_account_get_calendar_disabled (account))
    {
      g_autoptr(BjbProvider) provider = NULL;

      provider = bjb_nc_provider_new (object);
      g_signal_connect_object (provider, "item-removed",
                               G_CALLBACK (manager_provider_item_removed_cb),
                               self, G_CONNECT_SWAPPED);
      g_list_store_append (self->providers, provider);
      g_list_store_append (self->list_of_notes,
                           bjb_provider_get_notes (provider));
      dex_future_disown (bjb_provider_connect (provider));
    }
}

static void
manager_goa_account_removed_cb (BjbManager *self,
                                GoaObject  *object,
                                GoaClient  *client)
{
  GListModel *providers;
  guint n_items;

  g_assert (BJB_IS_MANAGER (self));
  g_assert (GOA_IS_CLIENT (client));
  g_assert (GOA_IS_OBJECT (object));

  providers = G_LIST_MODEL (self->providers);
  n_items = g_list_model_get_n_items (providers);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(BjbProvider) provider = NULL;

      provider = g_list_model_get_item (providers, i);

      if (BJB_IS_NC_PROVIDER (provider) &&
          bjb_nc_provider_matches_goa (BJB_NC_PROVIDER (provider), object))
        {
          guint position;

          if (g_list_store_find (self->providers, provider, &position))
            g_list_store_remove (self->providers, position);
          break;
        }
    }
}

static DexFuture *
bjb_manager_load_goa (gpointer data)
{
  BjbManager *self = data;
  g_autolist(GoaObject) accounts = NULL;

  g_signal_connect_object (self->goa_client, "account-added",
                           G_CALLBACK (manager_goa_account_added_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->goa_client, "account-removed",
                           G_CALLBACK (manager_goa_account_removed_cb),
                           self,
                           G_CONNECT_SWAPPED);

  accounts = goa_client_get_accounts (self->goa_client);

  for (GList *node = accounts; node; node = node->next)
    manager_goa_account_added_cb (self, node->data, self->goa_client);

  return dex_future_new_true ();
}

static DexFuture *
local_provider_connect_cb (DexFuture *completed,
                           gpointer   data)
{
  BjbManager *self = data;

  g_list_store_append (self->providers, self->local_provider);
  g_list_store_append (self->list_of_notes,
                       bjb_provider_get_notes (self->local_provider));
  g_list_store_append (self->list_of_notes,
                       bjb_provider_get_trash_notes (self->local_provider));

  return dex_future_new_true ();
}

static void
bjb_manager_finalize (GObject *object)
{
  BjbManager *self = (BjbManager *)object;

  g_clear_object (&self->list_of_notes);
  g_clear_object (&self->notes);

  g_clear_object (&self->eds_registry);
  g_clear_object (&self->goa_client);
  g_clear_object (&self->local_provider);
  g_clear_object (&self->providers);

  G_OBJECT_CLASS (bjb_manager_parent_class)->finalize (object);
}

static void
bjb_manager_class_init (BjbManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = bjb_manager_finalize;

  signals [ITEM_REMOVED] =
    g_signal_new ("item-removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  BJB_TYPE_PROVIDER, BJB_TYPE_ITEM);

}

static void
bjb_manager_init (BjbManager *self)
{
  self->providers = g_list_store_new (BJB_TYPE_PROVIDER);
  self->local_provider = bjb_local_provider_new (NULL);
  g_signal_connect_object (self->local_provider, "item-removed",
                           G_CALLBACK (manager_provider_item_removed_cb),
                           self, G_CONNECT_SWAPPED);

  self->list_of_notes = g_list_store_new (G_TYPE_LIST_MODEL);
  self->notes = gtk_flatten_list_model_new (g_object_ref (G_LIST_MODEL (self->list_of_notes)));
}

BjbManager *
bjb_manager_get_default (void)
{
  static BjbManager *self;

  if (!self)
    g_set_weak_pointer (&self, g_object_new (BJB_TYPE_MANAGER, NULL));

  return self;
}

DexFuture *
bjb_manager_load (BjbManager *self)
{
  DexFuture *local_future = NULL;
  DexFuture *eds_future = NULL;
  DexFuture *goa_future;
  BjbDex *dex;
  g_autoptr(GError) error = NULL;

  g_return_val_if_fail (BJB_IS_MANAGER (self), NULL);

  if (self->loaded)
    return dex_future_new_true ();

  g_assert (!self->is_loading);

  self->is_loading = TRUE;

  g_debug ("Loading Providers");


  dex = bjb_dex_new (NULL, e_source_registry_new_finish, G_TYPE_OBJECT);
  eds_future = bjb_dex_dup_future (dex);
  e_source_registry_new (bjb_dex_get_cancellable (dex),
                         bjb_dex_callback, dex);
  self->eds_registry = dex_await_object (eds_future, &error);

  if (error)
    eds_future = dex_future_new_for_error (error);
  else
    eds_future = bjb_manager_load_eds (self);


  dex = bjb_dex_new (NULL, goa_client_new_finish, G_TYPE_OBJECT);
  goa_future = bjb_dex_dup_future (dex);
  goa_client_new (bjb_dex_get_cancellable (dex),
                  bjb_dex_callback, dex);
  self->goa_client = dex_await_object (goa_future, &error);

  if (error)
    goa_future = dex_future_new_for_error (error);
  else
    goa_future = bjb_manager_load_goa (self);


  local_future = bjb_provider_connect (self->local_provider);
  local_future = dex_future_then (local_future,
                                  local_provider_connect_cb,
                                  g_object_ref (self),
                                  g_object_unref);

  return dex_future_all (local_future, eds_future, goa_future, NULL);
}

GListModel *
bjb_manager_get_providers (BjbManager *self)
{
  g_return_val_if_fail (BJB_IS_MANAGER (self), NULL);

  return G_LIST_MODEL (self->providers);
}


GListModel *
bjb_manager_get_notes (BjbManager *self)
{
  g_return_val_if_fail (BJB_IS_MANAGER (self), NULL);

  return G_LIST_MODEL (self->notes);
}
