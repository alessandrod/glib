/*
 * Copyright © 2009, 2010 Codethink Limited
 * Copyright © 2010 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the licence, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Ryan Lortie <desrt@desrt.ca>
 *          Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include "gsettingsbackendinternal.h"
#include "gsimplepermission.h"
#include "giomodule-priv.h"

#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <glibintl.h>


typedef struct _GSettingsBackendClosure GSettingsBackendClosure;
typedef struct _GSettingsBackendWatch   GSettingsBackendWatch;

struct _GSettingsBackendPrivate
{
  GSettingsBackendWatch *watches;
  GMutex lock;
};

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GSettingsBackend, g_settings_backend, G_TYPE_OBJECT)

/* For g_settings_backend_sync_default(), we only want to actually do
 * the sync if the backend already exists.  This avoids us creating an
 * entire GSettingsBackend in order to call a do-nothing sync()
 * operation on it.  This variable lets us avoid that.
 */
static gboolean g_settings_has_backend;

/**
 * SECTION:gsettingsbackend
 * @title: GSettingsBackend
 * @short_description: Interface for settings backend implementations
 * @include: gio/gsettingsbackend.h
 * @see_also: #GSettings, #GIOExtensionPoint
 *
 * The #GSettingsBackend interface defines a generic interface for
 * non-strictly-typed data that is stored in a hierarchy. To implement
 * an alternative storage backend for #GSettings, you need to implement
 * the #GSettingsBackend interface and then make it implement the
 * extension point #G_SETTINGS_BACKEND_EXTENSION_POINT_NAME.
 *
 * The interface defines methods for reading and writing values, a
 * method for determining if writing of certain values will fail
 * (lockdown) and a change notification mechanism.
 *
 * The semantics of the interface are very precisely defined and
 * implementations must carefully adhere to the expectations of
 * callers that are documented on each of the interface methods.
 *
 * Some of the GSettingsBackend functions accept or return a #GTree.
 * These trees always have strings as keys and #GVariant as values.
 * g_settings_backend_create_tree() is a convenience function to create
 * suitable trees.
 *
 * The GSettingsBackend API is exported to allow third-party
 * implementations, but does not carry the same stability guarantees
 * as the public GIO API. For this reason, you have to define the
 * C preprocessor symbol %G_SETTINGS_ENABLE_BACKEND before including
 * `gio/gsettingsbackend.h`.
 **/

static gboolean
is_key (const gchar *key)
{
  gint length;
  gint i;

  g_return_val_if_fail (key != NULL, FALSE);
  g_return_val_if_fail (key[0] == '/', FALSE);

  for (i = 1; key[i]; i++)
    g_return_val_if_fail (key[i] != '/' || key[i + 1] != '/', FALSE);

  length = i;

  g_return_val_if_fail (key[length - 1] != '/', FALSE);

  return TRUE;
}

static gboolean
is_path (const gchar *path)
{
  gint length;
  gint i;

  g_return_val_if_fail (path != NULL, FALSE);
  g_return_val_if_fail (path[0] == '/', FALSE);

  for (i = 1; path[i]; i++)
    g_return_val_if_fail (path[i] != '/' || path[i + 1] != '/', FALSE);

  length = i;

  g_return_val_if_fail (path[length - 1] == '/', FALSE);

  return TRUE;
}

struct _GSettingsBackendWatch
{
  GObject                       *target;
  const GSettingsListenerVTable *vtable;
  GMainContext                  *context;
  GSettingsBackendWatch         *next;
};

struct _GSettingsBackendClosure
{
  void (*function) (GObject           *target,
                    GSettingsBackend  *backend,
                    const gchar       *name,
                    gpointer           origin_tag,
                    gchar            **names);

  GMainContext      *context;
  GObject           *target;
  GSettingsBackend  *backend;
  gchar             *name;
  gpointer           origin_tag;
  gchar            **names;
};

static void
g_settings_backend_watch_weak_notify (gpointer  data,
                                      GObject  *where_the_object_was)
{
  GSettingsBackend *backend = data;
  GSettingsBackendWatch **ptr;

  /* search and remove */
  g_mutex_lock (&backend->priv->lock);
  for (ptr = &backend->priv->watches; *ptr; ptr = &(*ptr)->next)
    if ((*ptr)->target == where_the_object_was)
      {
        GSettingsBackendWatch *tmp = *ptr;

        *ptr = tmp->next;
        g_slice_free (GSettingsBackendWatch, tmp);

        g_mutex_unlock (&backend->priv->lock);
        return;
      }

  /* we didn't find it.  that shouldn't happen. */
  g_assert_not_reached ();
}

/*< private >
 * g_settings_backend_watch:
 * @backend: a #GSettingsBackend
 * @target: the GObject (typically GSettings instance) to call back to
 * @context: (allow-none): a #GMainContext, or %NULL
 * ...: callbacks...
 *
 * Registers a new watch on a #GSettingsBackend.
 *
 * note: %NULL @context does not mean "default main context" but rather,
 * "it is okay to dispatch in any context".  If the default main context
 * is specifically desired then it must be given.
 *
 * note also: if you want to get meaningful values for the @origin_tag
 * that appears as an argument to some of the callbacks, you *must* have
 * @context as %NULL.  Otherwise, you are subject to cross-thread
 * dispatching and whatever owned @origin_tag at the time that the event
 * occurred may no longer own it.  This is a problem if you consider that
 * you may now be the new owner of that address and mistakenly think
 * that the event in question originated from yourself.
 *
 * tl;dr: If you give a non-%NULL @context then you must ignore the
 * value of @origin_tag given to any callbacks.
 **/
void
g_settings_backend_watch (GSettingsBackend              *backend,
                          const GSettingsListenerVTable *vtable,
                          GObject                       *target,
                          GMainContext                  *context)
{
  GSettingsBackendWatch *watch;

  /* For purposes of discussion, we assume that our target is a
   * GSettings instance.
   *
   * Our strategy to defend against the final reference dropping on the
   * GSettings object in a thread other than the one that is doing the
   * dispatching is as follows:
   *
   *  1) hold a GObject reference on the GSettings during an outstanding
   *     dispatch.  This ensures that the delivery is always possible.
   *
   *  2) hold a weak reference on the GSettings at other times.  This
   *     allows us to receive early notification of pending destruction
   *     of the object.  At this point, it is still safe to obtain a
   *     reference on the GObject to keep it alive, so #1 will work up
   *     to that point.  After that point, we'll have been able to drop
   *     the watch from the list.
   *
   * Note, in particular, that it's not possible to simply have an
   * "unwatch" function that gets called from the finalize function of
   * the GSettings instance because, by that point it is no longer
   * possible to keep the object alive using g_object_ref() and we would
   * have no way of knowing this.
   *
   * Note also that we do not need to hold a reference on the main
   * context here since the GSettings instance does that for us and we
   * will receive the weak notify long before it is dropped.  We don't
   * even need to hold it during dispatches because our reference on the
   * GSettings will prevent the finalize from running and dropping the
   * ref on the context.
   *
   * All access to the list holds a mutex.  We have some strategies to
   * avoid some of the pain that would be associated with that.
   */

  watch = g_slice_new (GSettingsBackendWatch);
  watch->context = context;
  watch->vtable = vtable;
  watch->target = target;
  g_object_weak_ref (target, g_settings_backend_watch_weak_notify, backend);

  /* linked list prepend */
  g_mutex_lock (&backend->priv->lock);
  watch->next = backend->priv->watches;
  backend->priv->watches = watch;
  g_mutex_unlock (&backend->priv->lock);
}

void
g_settings_backend_unwatch (GSettingsBackend *backend,
                            GObject          *target)
{
  /* Our caller surely owns a reference on 'target', so the order of
   * these two calls is unimportant.
   */
  g_object_weak_unref (target, g_settings_backend_watch_weak_notify, backend);
  g_settings_backend_watch_weak_notify (backend, target);
}

static gboolean
g_settings_backend_invoke_closure (gpointer user_data)
{
  GSettingsBackendClosure *closure = user_data;

  closure->function (closure->target, closure->backend, closure->name,
                     closure->origin_tag, closure->names);

  g_object_unref (closure->backend);
  g_object_unref (closure->target);
  g_strfreev (closure->names);
  g_free (closure->name);

  g_slice_free (GSettingsBackendClosure, closure);

  return FALSE;
}

static void
g_settings_backend_dispatch_signal (GSettingsBackend    *backend,
                                    gsize                function_offset,
                                    const gchar         *name,
                                    gpointer             origin_tag,
                                    const gchar * const *names)
{
  GSettingsBackendWatch *watch;
  GSList *closures = NULL;

  /* We're in a little bit of a tricky situation here.  We need to hold
   * a lock while traversing the list, but we don't want to hold the
   * lock while calling back into user code.
   *
   * We work around this by creating a bunch of GSettingsBackendClosure
   * objects while holding the lock and dispatching them after.  We
   * never touch the list without holding the lock.
   */
  g_mutex_lock (&backend->priv->lock);
  for (watch = backend->priv->watches; watch; watch = watch->next)
    {
      GSettingsBackendClosure *closure;

      closure = g_slice_new (GSettingsBackendClosure);
      closure->context = watch->context;
      closure->backend = g_object_ref (backend);
      closure->target = g_object_ref (watch->target);
      closure->function = G_STRUCT_MEMBER (void *, watch->vtable,
                                           function_offset);
      closure->name = g_strdup (name);
      closure->origin_tag = origin_tag;
      closure->names = g_strdupv ((gchar **) names);

      closures = g_slist_prepend (closures, closure);
    }
  g_mutex_unlock (&backend->priv->lock);

  while (closures)
    {
      GSettingsBackendClosure *closure = closures->data;

      if (closure->context)
        g_main_context_invoke (closure->context,
                               g_settings_backend_invoke_closure,
                               closure);
      else
        g_settings_backend_invoke_closure (closure);

      closures = g_slist_delete_link (closures, closures);
    }
}

/**
 * g_settings_backend_changed:
 * @backend: a #GSettingsBackend implementation
 * @key: the name of the key
 * @origin_tag: the origin tag
 *
 * Signals that a single key has possibly changed.  Backend
 * implementations should call this if a key has possibly changed its
 * value.
 *
 * @key must be a valid key (ie starting with a slash, not containing
 * '//', and not ending with a slash).
 *
 * The implementation must call this function during any call to
 * g_settings_backend_write(), before the call returns (except in the
 * case that no keys are actually changed and it cares to detect this
 * fact).  It may not rely on the existence of a mainloop for
 * dispatching the signal later.
 *
 * The implementation may call this function at any other time it likes
 * in response to other events (such as changes occurring outside of the
 * program).  These calls may originate from a mainloop or may originate
 * in response to any other action (including from calls to
 * g_settings_backend_write()).
 *
 * In the case that this call is in response to a call to
 * g_settings_backend_write() then @origin_tag must be set to the same
 * value that was passed to that call.
 *
 * Since: 2.26
 **/
void
g_settings_backend_changed (GSettingsBackend *backend,
                            const gchar      *key,
                            gpointer          origin_tag)
{
  g_return_if_fail (G_IS_SETTINGS_BACKEND (backend));
  g_return_if_fail (is_key (key));

  g_settings_backend_dispatch_signal (backend,
                                      G_STRUCT_OFFSET (GSettingsListenerVTable,
                                                       changed),
                                      key, origin_tag, NULL);
}

/**
 * g_settings_backend_keys_changed:
 * @backend: a #GSettingsBackend implementation
 * @path: the path containing the changes
 * @items: (array zero-terminated=1): the %NULL-terminated list of changed keys
 * @origin_tag: the origin tag
 *
 * Signals that a list of keys have possibly changed.  Backend
 * implementations should call this if keys have possibly changed their
 * values.
 *
 * @path must be a valid path (ie starting and ending with a slash and
 * not containing '//').  Each string in @items must form a valid key
 * name when @path is prefixed to it (ie: each item must not start or
 * end with '/' and must not contain '//').
 *
 * The meaning of this signal is that any of the key names resulting
 * from the contatenation of @path with each item in @items may have
 * changed.
 *
 * The same rules for when notifications must occur apply as per
 * g_settings_backend_changed().  These two calls can be used
 * interchangeably if exactly one item has changed (although in that
 * case g_settings_backend_changed() is definitely preferred).
 *
 * For efficiency reasons, the implementation should strive for @path to
 * be as long as possible (ie: the longest common prefix of all of the
 * keys that were changed) but this is not strictly required.
 *
 * Since: 2.26
 */
void
g_settings_backend_keys_changed (GSettingsBackend    *backend,
                                 const gchar         *path,
                                 gchar const * const *items,
                                 gpointer             origin_tag)
{
  g_return_if_fail (G_IS_SETTINGS_BACKEND (backend));
  g_return_if_fail (is_path (path));

  /* XXX: should do stricter checking (ie: inspect each item) */
  g_return_if_fail (items != NULL);

  g_settings_backend_dispatch_signal (backend,
                                      G_STRUCT_OFFSET (GSettingsListenerVTable,
                                                       keys_changed),
                                      path, origin_tag, items);
}

/**
 * g_settings_backend_path_changed:
 * @backend: a #GSettingsBackend implementation
 * @path: the path containing the changes
 * @origin_tag: the origin tag
 *
 * Signals that all keys below a given path may have possibly changed.
 * Backend implementations should call this if an entire path of keys
 * have possibly changed their values.
 *
 * @path must be a valid path (ie starting and ending with a slash and
 * not containing '//').
 *
 * The meaning of this signal is that any of the key which has a name
 * starting with @path may have changed.
 *
 * The same rules for when notifications must occur apply as per
 * g_settings_backend_changed().  This call might be an appropriate
 * reasponse to a 'reset' call but implementations are also free to
 * explicitly list the keys that were affected by that call if they can
 * easily do so.
 *
 * For efficiency reasons, the implementation should strive for @path to
 * be as long as possible (ie: the longest common prefix of all of the
 * keys that were changed) but this is not strictly required.  As an
 * example, if this function is called with the path of "/" then every
 * single key in the application will be notified of a possible change.
 *
 * Since: 2.26
 */
void
g_settings_backend_path_changed (GSettingsBackend *backend,
                                 const gchar      *path,
                                 gpointer          origin_tag)
{
  g_return_if_fail (G_IS_SETTINGS_BACKEND (backend));
  g_return_if_fail (is_path (path));

  g_settings_backend_dispatch_signal (backend,
                                      G_STRUCT_OFFSET (GSettingsListenerVTable,
                                                       path_changed),
                                      path, origin_tag, NULL);
}

/**
 * g_settings_backend_writable_changed:
 * @backend: a #GSettingsBackend implementation
 * @key: the name of the key
 *
 * Signals that the writability of a single key has possibly changed.
 *
 * Since GSettings performs no locking operations for itself, this call
 * will always be made in response to external events.
 *
 * Since: 2.26
 **/
void
g_settings_backend_writable_changed (GSettingsBackend *backend,
                                     const gchar      *key)
{
  g_return_if_fail (G_IS_SETTINGS_BACKEND (backend));
  g_return_if_fail (is_key (key));

  g_settings_backend_dispatch_signal (backend,
                                      G_STRUCT_OFFSET (GSettingsListenerVTable,
                                                       writable_changed),
                                      key, NULL, NULL);
}

/**
 * g_settings_backend_path_writable_changed:
 * @backend: a #GSettingsBackend implementation
 * @path: the name of the path
 *
 * Signals that the writability of all keys below a given path may have
 * changed.
 *
 * Since GSettings performs no locking operations for itself, this call
 * will always be made in response to external events.
 *
 * Since: 2.26
 **/
void
g_settings_backend_path_writable_changed (GSettingsBackend *backend,
                                          const gchar      *path)
{
  g_return_if_fail (G_IS_SETTINGS_BACKEND (backend));
  g_return_if_fail (is_path (path));

  g_settings_backend_dispatch_signal (backend,
                                      G_STRUCT_OFFSET (GSettingsListenerVTable,
                                                       path_writable_changed),
                                      path, NULL, NULL);
}

void
g_settings_backend_flatten_tree (GTree         *tree,
                                 gchar        **path,
                                 const gchar ***keys,
                                 GVariant    ***values)
{
  g_assert_not_reached ();
}

/**
 * g_settings_backend_changeset_applied:
 * @backend: a #GSettingsBackend implementation
 * @changeset: the #GSettingsBackendChangeset corresponding to the change
 * @origin_tag: the origin tag
 *
 * This call is a convenience wrapper.  It gets the list of changes from
 * the changeset and emits the correct set of change signals.  If the
 * changeset is not already sealed, then calling this function will seal
 * it.
 *
 * Since: 2.26
 **/
void
g_settings_backend_changed_tree (GSettingsBackend *backend,
                                 GTree            *tree,
                                 gpointer          origin_tag)
{
  g_assert_not_reached ();
}


void
g_settings_backend_changeset_applied (GSettingsBackend          *backend,
                                      GSettingsBackendChangeset *changeset,
                                      gpointer                   origin_tag)
{
  const gchar * const *paths;
  const gchar *prefix;
  guint n_items;

  g_return_if_fail (G_IS_SETTINGS_BACKEND (backend));

  n_items = g_settings_backend_changeset_describe (changeset, &prefix, &paths, NULL);

  if (n_items == 1)
    {
      g_assert (paths[0][0] == '\0');
      g_settings_backend_changed (backend, prefix, origin_tag);
    }

  else if (n_items > 1)
    g_settings_backend_keys_changed (backend, prefix, paths, origin_tag);
}

/*< private >
 * g_settings_backend_read_value:
 * @backend: a #GSettingsBackend implementation
 * @key: the key to read
 * @expected_type: a #GVariantType
 * @read_through: a #GQueue of #GSettingsBackendChangeset
 *
 * Reads a key. This call will never block.
 *
 * If the key exists, the value associated with it will be returned.
 * If the key does not exist, %NULL will be returned.
 *
 * The returned value will be of the type given in @expected_type.  If
 * the backend stored a value of a different type then %NULL will be
 * returned.
 *
 * If @read_through is given then the read is performed as if the
 * changesets in the queue had first been applied to the underlying
 * backend.  @read_through may be modified during the duration of this
 * call but it will be returned to its original value before the call
 * returns.
 *
 * Unlike the backend vfuncs, this function will always return a value
 * of the correct type.  If the backend returned an incorrect type then
 * this function will return %NULL.
 *
 * Returns: the value that was read, or %NULL
 */
GVariant *
g_settings_backend_read_value (GSettingsBackend   *backend,
                               const gchar        *key,
                               const GVariantType *expected_type,
                               GQueue             *read_through,
                               gboolean            user_value_only,
                               gboolean            default_value)
{
  GVariant *value;

  value = G_SETTINGS_BACKEND_GET_CLASS (backend)->read_value (backend, key, expected_type,
                                                              read_through, user_value_only, default_value);

  if (value != NULL)
    value = g_variant_take_ref (value);

  if G_UNLIKELY (value && !g_variant_is_of_type (value, expected_type))
    {
      g_variant_unref (value);
      value = NULL;
    }

  return value;
}

/*< private >
 * g_settings_backend_read_user_value:
 * @backend: a #GSettingsBackend implementation
 * @key: the key to read
 * @expected_type: a #GVariantType
 *
 * Reads the 'user value' of a key.
 *
 * This is the value of the key that the user has control over and has
 * set for themselves.  Put another way: if the user did not set the
 * value for themselves, then this will return %NULL (even if the
 * sysadmin has provided a default value).
 *
 * Returns: the value that was read, or %NULL
 */
GVariant *
g_settings_backend_read_user_value (GSettingsBackend   *backend,
                                    const gchar        *key,
                                    const GVariantType *expected_type)
{
  GVariant *value;

  value = G_SETTINGS_BACKEND_GET_CLASS (backend)
    ->read_user_value (backend, key, expected_type);

  if (value != NULL)
    value = g_variant_take_ref (value);

  if G_UNLIKELY (value && !g_variant_is_of_type (value, expected_type))
    {
      g_variant_unref (value);
      value = NULL;
    }

  return value;
}

/*< private >
 * g_settings_backend_write:
 * @backend: a #GSettingsBackend implementation
 * @key: the name of the key
 * @value: a #GVariant value to write to this key
 * @origin_tag: the origin tag
 *
 * Writes exactly one key.
 *
 * This call does not fail.  During this call a
 * #GSettingsBackend::changed signal will be emitted if the value of the
 * key has changed.  The updated key value will be visible to any signal
 * callbacks.
 *
 * One possible method that an implementation might deal with failures is
 * to emit a second "changed" signal (either during this call, or later)
 * to indicate that the affected keys have suddenly "changed back" to their
 * old values.
 *
 * Returns: %TRUE if the write succeeded, %FALSE if the key was not writable
 */
gboolean
g_settings_backend_write (GSettingsBackend *backend,
                          const gchar      *key,
                          GVariant         *value,
                          gpointer          origin_tag)
{
  gboolean success;

  g_variant_ref_sink (value);
  success = G_SETTINGS_BACKEND_GET_CLASS (backend)
    ->write (backend, key, value, origin_tag);
  g_variant_unref (value);

  return success;
}

/*< private >
 * g_settings_backend_write_tree:
 * @backend: a #GSettingsBackend implementation
 * @changeset: a #GSettingsBackendChangeset containing the change
 * @origin_tag: the origin tag
 *
 * Writes one or more keys.  This call will never block.
 *
 * During this call a #GSettingsBackend::changed signal will be emitted
 * if any keys have been changed.  The new values of all updated keys
 * will be visible to any signal callbacks.
 *
 * One possible method that an implementation might deal with failures is
 * to emit a second "changed" signal (either during this call, or later)
 * to indicate that the affected keys have suddenly "changed back" to their
 * old values.
 */
gboolean
g_settings_backend_write_changeset (GSettingsBackend          *backend,
                                    GSettingsBackendChangeset *changeset,
                                    gpointer                   origin_tag)
{
  return G_SETTINGS_BACKEND_GET_CLASS (backend)->write_changeset (backend, changeset, origin_tag);
}

/*< private >
 * g_settings_backend_reset:
 * @backend: a #GSettingsBackend implementation
 * @key: the name of a key
 * @origin_tag: the origin tag
 *
 * "Resets" the named key to its "default" value (ie: after system-wide
 * defaults, mandatory keys, etc. have been taken into account) or possibly
 * unsets it.
 */
void
g_settings_backend_reset (GSettingsBackend *backend,
                          const gchar      *key,
                          gpointer          origin_tag)
{
  G_SETTINGS_BACKEND_GET_CLASS (backend)
    ->reset (backend, key, origin_tag);
}

/*< private >
 * g_settings_backend_get_writable:
 * @backend: a #GSettingsBackend implementation
 * @key: the name of a key
 *
 * Finds out if a key is available for writing to.  This is the
 * interface through which 'lockdown' is implemented.  Locked down
 * keys will have %FALSE returned by this call.
 *
 * You should not write to locked-down keys, but if you do, the
 * implementation will deal with it.
 *
 * Returns: %TRUE if the key is writable
 */
gboolean
g_settings_backend_get_writable (GSettingsBackend *backend,
                                 const gchar      *key)
{
  return G_SETTINGS_BACKEND_GET_CLASS (backend)
    ->get_writable (backend, key);
}

/*< private >
 * g_settings_backend_unsubscribe:
 * @backend: a #GSettingsBackend
 * @name: a key or path to subscribe to
 *
 * Reverses the effect of a previous call to
 * g_settings_backend_subscribe().
 */
void
g_settings_backend_unsubscribe (GSettingsBackend *backend,
                                const char       *name)
{
  G_SETTINGS_BACKEND_GET_CLASS (backend)
    ->unsubscribe (backend, name);
}

/*< private >
 * g_settings_backend_subscribe:
 * @backend: a #GSettingsBackend
 * @name: a key or path to subscribe to
 *
 * Requests that change signals be emitted for events on @name.
 */
void
g_settings_backend_subscribe (GSettingsBackend *backend,
                              const gchar      *name)
{
  G_SETTINGS_BACKEND_GET_CLASS (backend)
    ->subscribe (backend, name);
}

static GVariant *
g_settings_backend_real_read_value (GSettingsBackend   *backend,
                                    const gchar        *key,
                                    const GVariantType *expected_type,
                                    GQueue             *read_through,
                                    gboolean            user_value_only,
                                    gboolean            default_value)
{
  GVariant *value = NULL;

  if (default_value)
    return G_SETTINGS_BACKEND_GET_CLASS (backend)->read (backend, key, expected_type, TRUE);

  if (g_settings_backend_check_changeset_queue (read_through, key, &value))
    return value;

  if (user_value_only)
    return G_SETTINGS_BACKEND_GET_CLASS (backend)->read_user_value (backend, key, expected_type);

  return G_SETTINGS_BACKEND_GET_CLASS (backend)->read (backend, key, expected_type, FALSE);
}

static gboolean
g_settings_backend_real_write (GSettingsBackend *backend,
                               const gchar      *key,
                               GVariant         *value,
                               gpointer          origin_tag)
{
  GSettingsBackendChangeset *changeset;
  gboolean success;

  changeset = g_settings_backend_changeset_new_write (key, value);
  success = g_settings_backend_write_changeset (backend, changeset, origin_tag);
  g_settings_backend_changeset_unref (changeset);

  return success;
}

static void
g_settings_backend_real_reset (GSettingsBackend *backend,
                               const gchar      *key,
                               gpointer          origin_tag)
{
  gboolean success;

  success = g_settings_backend_real_write (backend, key, NULL, origin_tag);

  if (!success)
    g_critical ("%s is behaving incorrectly: reset() must always succeed",
               g_type_name (G_TYPE_FROM_INSTANCE (backend)));
}

static gboolean
add_to_tree (const gchar *key,
             GVariant    *value,
             gpointer     user_data)
{
  g_tree_insert (user_data, (gpointer) key, value);

  return TRUE;
}

static gboolean
g_settings_backend_real_write_changeset (GSettingsBackend          *backend,
                                         GSettingsBackendChangeset *changeset,
                                         gpointer                   origin_tag)
{
  gboolean success;
  GTree *tree;

  tree = g_tree_new ((GCompareFunc) strcmp);

  g_settings_backend_changeset_all (changeset, add_to_tree, tree);

  success = G_SETTINGS_BACKEND_GET_CLASS (backend)->write_tree (backend, tree, origin_tag);

  g_tree_unref (tree);

  return success;
}

static gboolean
g_settings_backend_real_get_writable (GSettingsBackend *backend,
                                      const gchar      *key)
{
  return TRUE;
}

static void
g_settings_backend_finalize (GObject *object)
{
  GSettingsBackend *backend = G_SETTINGS_BACKEND (object);

  g_mutex_clear (&backend->priv->lock);

  G_OBJECT_CLASS (g_settings_backend_parent_class)
    ->finalize (object);
}

static void
ignore_subscription (GSettingsBackend *backend,
                     const gchar      *key)
{
}

static void
g_settings_backend_init (GSettingsBackend *backend)
{
  backend->priv = g_settings_backend_get_instance_private (backend);
  g_mutex_init (&backend->priv->lock);
}

static void
g_settings_backend_class_init (GSettingsBackendClass *class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);

  class->reset = g_settings_backend_real_reset;
  class->write = g_settings_backend_real_write;
  class->write_changeset = g_settings_backend_real_write_changeset;
  class->read_value = g_settings_backend_real_read_value;
  class->get_writable = g_settings_backend_real_get_writable;
  class->subscribe = ignore_subscription;
  class->unsubscribe = ignore_subscription;

  gobject_class->finalize = g_settings_backend_finalize;
}

static gboolean
g_settings_backend_verify (gpointer impl)
{
  GSettingsBackend *backend = impl;

  if (strcmp (G_OBJECT_TYPE_NAME (backend), "GMemorySettingsBackend") == 0 &&
      g_strcmp0 (g_getenv ("GSETTINGS_BACKEND"), "memory") != 0)
    {
      g_message ("Using the 'memory' GSettings backend.  Your settings "
		 "will not be saved or shared with other applications.");
    }

  g_settings_has_backend = TRUE;
  return TRUE;
}

/**
 * g_settings_backend_get_default:
 *
 * Returns the default #GSettingsBackend. It is possible to override
 * the default by setting the `GSETTINGS_BACKEND` environment variable
 * to the name of a settings backend.
 *
 * The user gets a reference to the backend.
 *
 * Returns: (transfer full): the default #GSettingsBackend
 *
 * Since: 2.28
 */
GSettingsBackend *
g_settings_backend_get_default (void)
{
  GSettingsBackend *backend;

  backend = _g_io_module_get_default (G_SETTINGS_BACKEND_EXTENSION_POINT_NAME,
				      "GSETTINGS_BACKEND",
				      g_settings_backend_verify);
  return g_object_ref (backend);
}

/*< private >
 * g_settings_backend_sync_default:
 *
 * Syncs the default backend.
 */
void
g_settings_backend_sync_default (void)
{
  if (g_settings_has_backend)
    {
      GSettingsBackendClass *class;
      GSettingsBackend *backend;

      backend = g_settings_backend_get_default ();
      class = G_SETTINGS_BACKEND_GET_CLASS (backend);

      if (class->sync)
        class->sync (backend);
    }
}

/**
 * g_settings_backend_is_path:
 * @string: a string
 *
 * Checks if @string is a valid #GSettingsBackend path.  Paths must
 * start with '/' and not contain '//'.
 *
 * A path may be either a key or a dir.  See g_settings_backend_is_key()
 * and g_settings_backend_is_dir() for examples of each.
 *
 * Returns: %TRUE if @string is a path
 **/
gboolean
g_settings_backend_is_path (const gchar *string)
{
  if (string[0] != '/')
    return FALSE;

  if (strstr (string, "//"))
    return FALSE;

  return TRUE;
}

/**
 * g_settings_backend_is_key:
 * @string: a string
 *
 * Checks if @string is a valid #GSettingsBackend key.  Keys must
 * start with '/', not contain '//' and not end with '/'.
 *
 * A key is the potential location of a single value within a
 * #GSettingsBackend.
 *
 * "/a", "/a/b" and "/a/b/c" are examples of keys.  "", "/", "a", "a/b",
 * "//a/b", "/a//b", and "/a/" are examples of strings that are not
 * keys.
 *
 * Returns: %TRUE if @string is a key
 **/
gboolean
g_settings_backend_is_key (const gchar *string)
{
  if (string[0] != '/')
    return FALSE;

  if (strstr (string, "//"))
    return FALSE;

  if (g_str_has_suffix (string, "/"))
    return FALSE;

  return TRUE;
}

/**
 * g_settings_backend_is_dir:
 * @string: a string
 *
 * Checks if @string is a valid #GSettingsBackend dir.  dirs must start
 * and end with '/' and not contain '//'.
 *
 * A dir refers to a subtree of the database that can contain other dirs
 * or keys.  If @string is a dir, then it will be a prefix of any key or
 * dir contained within it.
 *
 * "/", "/a/" and "/a/b/" are examples of dirs.  "", "a/", "a/b/",
 * "//a/b/", "/a//b/" and "/a" are examples of strings that are not
 * dirs.
 *
 * Returns: %TRUE if @string is a dir
 **/
gboolean
g_settings_backend_is_dir (const gchar *string)
{
  if (string[0] != '/')
    return FALSE;

  if (strstr (string, "//"))
    return FALSE;

  if (!g_str_has_suffix (string, "/"))
    return FALSE;

  return TRUE;
}

gboolean
g_settings_backend_check_changeset_queue (const GQueue  *queue,
                                          const gchar   *key,
                                          GVariant     **value)
{
  GList *link;

  if (queue == NULL)
    return FALSE;

  for (link = queue->tail; link; link = link->prev)
    if (g_settings_backend_changeset_get (link->data, key, value))
      return TRUE;

  return FALSE;
}
