/* bjb-dex.c
 *
 * Copyright 2025 Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "bjb-dex"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "bjb-dex.h"

struct _BjbDex
{
  GObject    parent_instance;

  gpointer    instance;
  gpointer    finish_func;

  DexPromise *promise;
  GType       return_type;
};


G_DEFINE_TYPE (BjbDex, bjb_dex, G_TYPE_OBJECT)

static void
bjb_dex_finalize (GObject *object)
{
  BjbDex *self = (BjbDex *)object;

  g_clear_object (&self->instance);
  g_clear_pointer (&self->promise, dex_unref);

  G_OBJECT_CLASS (bjb_dex_parent_class)->finalize (object);
}

static void
bjb_dex_class_init (BjbDexClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = bjb_dex_finalize;
}

static void
bjb_dex_init (BjbDex *self)
{
  self->promise = dex_promise_new_cancellable ();
}

/**
 * bjb_dex_new:
 * @instance: (nullable): A #GObject, to be used in callback
 * @finish_func: (nullable): The finish function to run in callback
 * @return_type: A #GType, the return type of @finish_func
 *
 * if @intance is %NULL, the finish_func shall be executed as
 * finish_func(result, error), otherwise, finish_func(instance, result, error)
 * shall be used.
 *
 * @instance and @finish_func can be %NULL if @return_type is
 * G_TYPE_ASYNC_RESULT, which will return the #GAsyncResult and
 * you can call the finish function on it (may be unsafe to do so).
 *
 * Returns: (transfer full): A #BjbDex
 */
BjbDex *
bjb_dex_new (gpointer instance,
             gpointer finish_func,
             GType    return_type)
{
  BjbDex *self;

  self = g_object_new (BJB_TYPE_DEX, NULL);
  g_set_object (&self->instance, instance);
  self->return_type = return_type;
  self->finish_func = finish_func;

  return self;
}

DexFuture *
bjb_dex_dup_future (BjbDex *self)
{
  g_return_val_if_fail (BJB_IS_DEX (self), NULL);

  return dex_ref (self->promise);
}

GCancellable *
bjb_dex_get_cancellable (BjbDex *self)
{
  g_return_val_if_fail (BJB_IS_DEX (self), NULL);

  return dex_promise_get_cancellable (self->promise);
}

/* bjb_dex_callback:
 *
 * The function to be used as callback for
 * async functions. @self should be passed
 * as user_data, which is (transfer full),
 * and will be freed.
 */
void
bjb_dex_callback (GObject      *object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr(BjbDex) self = user_data;
  GObject *value_object = NULL;
  GError *error = NULL;
  gboolean value_bool = FALSE;

  g_assert (BJB_IS_DEX (self));

  /* Probably not safe */
  if (self->return_type == G_TYPE_ASYNC_RESULT)
    {
      dex_promise_resolve_object (self->promise, g_object_ref (result));
      return;
    }

/* Adapted from libdex */
#define FINISH_WITH_OBJ(_d, TYPE) \
  (((TYPE (*) (gpointer, GAsyncResult*, GError**))_d->finish_func) (_d->instance, result, &error))
#define FINISH(_d, TYPE) \
  (((TYPE (*) (GAsyncResult*, GError**))_d->finish_func) (result, &error))

  switch (self->return_type)
    {
    case G_TYPE_BOOLEAN:
      if (self->instance)
        value_bool = FINISH_WITH_OBJ (self, gboolean);
      else
        value_bool = FINISH (self, gboolean);
      break;

    case G_TYPE_OBJECT:
      if (self->instance)
        value_object = FINISH_WITH_OBJ (self, gpointer);
      else
        value_object = FINISH (self, gpointer);
      break;

    default:
      error = g_error_new (DEX_ERROR,
                           DEX_ERROR_TYPE_NOT_SUPPORTED,
                           "Type '%s' is not currently supported by BjbDex",
                           g_type_name (self->return_type));
    }

  if (error)
    dex_promise_reject (self->promise, error);
  else if (self->return_type == G_TYPE_BOOLEAN)
    dex_promise_resolve_boolean (self->promise, value_bool);
  else if (self->return_type == G_TYPE_OBJECT)
    dex_promise_resolve_object (self->promise, value_object);
  else
    g_assert_not_reached ();
}
