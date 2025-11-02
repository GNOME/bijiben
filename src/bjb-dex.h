/* bjb-dex.h
 *
 * Copyright 2025 Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */


#pragma once

#include <gio/gio.h>
#include <libdex.h>

G_BEGIN_DECLS

#define BJB_TYPE_DEX (bjb_dex_get_type ())
G_DECLARE_FINAL_TYPE (BjbDex, bjb_dex, BJB, DEX, GObject)

BjbDex *bjb_dex_new                   (gpointer      instance,
                                       gpointer      finish_func,
                                       GType         return_type);
DexFuture    *bjb_dex_dup_future      (BjbDex       *self);
GCancellable *bjb_dex_get_cancellable (BjbDex       *self);
void          bjb_dex_callback        (GObject      *object,
                                       GAsyncResult *result,
                                       gpointer      user_data);

G_END_DECLS
