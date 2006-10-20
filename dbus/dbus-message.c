/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-message.c  DBusMessage object
 *
 * Copyright (C) 2002, 2003, 2004, 2005  Red Hat Inc.
 * Copyright (C) 2002, 2003  CodeFactory AB
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-internals.h"
#include "dbus-marshal-recursive.h"
#include "dbus-marshal-validate.h"
#include "dbus-marshal-byteswap.h"
#include "dbus-marshal-header.h"
#include "dbus-signature.h"
#include "dbus-message-private.h"
#include "dbus-object-tree.h"
#include "dbus-memory.h"
#include "dbus-list.h"
#include "dbus-threads-internal.h"
#include <string.h>

static void dbus_message_finalize (DBusMessage *message);

/**
 * @defgroup DBusMessageInternals DBusMessage implementation details
 * @ingroup DBusInternals
 * @brief DBusMessage private implementation details.
 *
 * The guts of DBusMessage and its methods.
 *
 * @{
 */

/* Not thread locked, but strictly const/read-only so should be OK
 */
/** An static string representing an empty signature */
_DBUS_STRING_DEFINE_STATIC(_dbus_empty_signature_str,  "");

/* these have wacky values to help trap uninitialized iterators;
 * but has to fit in 3 bits
 */
enum {
  DBUS_MESSAGE_ITER_TYPE_READER = 3,
  DBUS_MESSAGE_ITER_TYPE_WRITER = 7
};

/** typedef for internals of message iterator */
typedef struct DBusMessageRealIter DBusMessageRealIter;

/**
 * @brief Internals of DBusMessageIter
 *
 * Object representing a position in a message. All fields are internal.
 */
struct DBusMessageRealIter
{
  DBusMessage *message; /**< Message used */
  dbus_uint32_t changed_stamp : CHANGED_STAMP_BITS; /**< stamp to detect invalid iters */
  dbus_uint32_t iter_type : 3;      /**< whether this is a reader or writer iter */
  dbus_uint32_t sig_refcount : 8;   /**< depth of open_signature() */
  union
  {
    DBusTypeWriter writer; /**< writer */
    DBusTypeReader reader; /**< reader */
  } u; /**< the type writer or reader that does all the work */
};

static void
get_const_signature (DBusHeader        *header,
                     const DBusString **type_str_p,
                     int               *type_pos_p)
{
  if (_dbus_header_get_field_raw (header,
                                  DBUS_HEADER_FIELD_SIGNATURE,
                                  type_str_p,
                                  type_pos_p))
    {
      *type_pos_p += 1; /* skip the signature length which is 1 byte */
    }
  else
    {
      *type_str_p = &_dbus_empty_signature_str;
      *type_pos_p = 0;
    }
}

/**
 * Swaps the message to compiler byte order if required
 *
 * @param message the message
 */
static void
_dbus_message_byteswap (DBusMessage *message)
{
  const DBusString *type_str;
  int type_pos;
  
  if (message->byte_order == DBUS_COMPILER_BYTE_ORDER)
    return;

  _dbus_verbose ("Swapping message into compiler byte order\n");
  
  get_const_signature (&message->header, &type_str, &type_pos);
  
  _dbus_marshal_byteswap (type_str, type_pos,
                          message->byte_order,
                          DBUS_COMPILER_BYTE_ORDER,
                          &message->body, 0);

  message->byte_order = DBUS_COMPILER_BYTE_ORDER;
  
  _dbus_header_byteswap (&message->header, DBUS_COMPILER_BYTE_ORDER);
}

/** byte-swap the message if it doesn't match our byte order.
 *  Called only when we need the message in our own byte order,
 *  normally when reading arrays of integers or doubles.
 *  Otherwise should not be called since it would do needless
 *  work.
 */
#define ensure_byte_order(message)                      \
 if (message->byte_order != DBUS_COMPILER_BYTE_ORDER)   \
   _dbus_message_byteswap (message)

/**
 * Gets the data to be sent over the network for this message.
 * The header and then the body should be written out.
 * This function is guaranteed to always return the same
 * data once a message is locked (with _dbus_message_lock()).
 *
 * @param message the message.
 * @param header return location for message header data.
 * @param body return location for message body data.
 */
void
_dbus_message_get_network_data (DBusMessage          *message,
                                const DBusString    **header,
                                const DBusString    **body)
{
  _dbus_assert (message->locked);

  *header = &message->header.data;
  *body = &message->body;
}

/**
 * Sets the serial number of a message.
 * This can only be done once on a message.
 *
 * @param message the message
 * @param serial the serial
 */
void
_dbus_message_set_serial (DBusMessage   *message,
                          dbus_uint32_t  serial)
{
  _dbus_assert (message != NULL);
  _dbus_assert (!message->locked);
  _dbus_assert (dbus_message_get_serial (message) == 0);

  _dbus_header_set_serial (&message->header, serial);
}

/**
 * Adds a counter to be incremented immediately with the
 * size of this message, and decremented by the size
 * of this message when this message if finalized.
 * The link contains a counter with its refcount already
 * incremented, but the counter itself not incremented.
 * Ownership of link and counter refcount is passed to
 * the message.
 *
 * @param message the message
 * @param link link with counter as data
 */
void
_dbus_message_add_size_counter_link (DBusMessage  *message,
                                     DBusList     *link)
{
  /* right now we don't recompute the delta when message
   * size changes, and that's OK for current purposes
   * I think, but could be important to change later.
   * Do recompute it whenever there are no outstanding counters,
   * since it's basically free.
   */
  if (message->size_counters == NULL)
    {
      message->size_counter_delta =
        _dbus_string_get_length (&message->header.data) +
        _dbus_string_get_length (&message->body);

#if 0
      _dbus_verbose ("message has size %ld\n",
                     message->size_counter_delta);
#endif
    }

  _dbus_list_append_link (&message->size_counters, link);

  _dbus_counter_adjust (link->data, message->size_counter_delta);
}

/**
 * Adds a counter to be incremented immediately with the
 * size of this message, and decremented by the size
 * of this message when this message if finalized.
 *
 * @param message the message
 * @param counter the counter
 * @returns #FALSE if no memory
 */
dbus_bool_t
_dbus_message_add_size_counter (DBusMessage *message,
                                DBusCounter *counter)
{
  DBusList *link;

  link = _dbus_list_alloc_link (counter);
  if (link == NULL)
    return FALSE;

  _dbus_counter_ref (counter);
  _dbus_message_add_size_counter_link (message, link);

  return TRUE;
}

/**
 * Removes a counter tracking the size of this message, and decrements
 * the counter by the size of this message.
 *
 * @param message the message
 * @param link_return return the link used
 * @param counter the counter
 */
void
_dbus_message_remove_size_counter (DBusMessage  *message,
                                   DBusCounter  *counter,
                                   DBusList    **link_return)
{
  DBusList *link;

  link = _dbus_list_find_last (&message->size_counters,
                               counter);
  _dbus_assert (link != NULL);

  _dbus_list_unlink (&message->size_counters,
                     link);
  if (link_return)
    *link_return = link;
  else
    _dbus_list_free_link (link);

  _dbus_counter_adjust (counter, - message->size_counter_delta);

  _dbus_counter_unref (counter);
}

/**
 * Locks a message. Allows checking that applications don't keep a
 * reference to a message in the outgoing queue and change it
 * underneath us. Messages are locked when they enter the outgoing
 * queue (dbus_connection_send_message()), and the library complains
 * if the message is modified while locked.
 *
 * @param message the message to lock.
 */
void
_dbus_message_lock (DBusMessage  *message)
{
  if (!message->locked)
    {
      _dbus_header_update_lengths (&message->header,
                                   _dbus_string_get_length (&message->body));

      /* must have a signature if you have a body */
      _dbus_assert (_dbus_string_get_length (&message->body) == 0 ||
                    dbus_message_get_signature (message) != NULL);

      message->locked = TRUE;
    }
}

static dbus_bool_t
set_or_delete_string_field (DBusMessage *message,
                            int          field,
                            int          typecode,
                            const char  *value)
{
  if (value == NULL)
    return _dbus_header_delete_field (&message->header, field);
  else
    return _dbus_header_set_field_basic (&message->header,
                                         field,
                                         typecode,
                                         &value);
}

#if 0
/* Probably we don't need to use this */
/**
 * Sets the signature of the message, i.e. the arguments in the
 * message payload. The signature includes only "in" arguments for
 * #DBUS_MESSAGE_TYPE_METHOD_CALL and only "out" arguments for
 * #DBUS_MESSAGE_TYPE_METHOD_RETURN, so is slightly different from
 * what you might expect (it does not include the signature of the
 * entire C++-style method).
 *
 * The signature is a string made up of type codes such as
 * #DBUS_TYPE_INT32. The string is terminated with nul (nul is also
 * the value of #DBUS_TYPE_INVALID). The macros such as
 * #DBUS_TYPE_INT32 evaluate to integers; to assemble a signature you
 * may find it useful to use the string forms, such as
 * #DBUS_TYPE_INT32_AS_STRING.
 *
 * An "unset" or #NULL signature is considered the same as an empty
 * signature. In fact dbus_message_get_signature() will never return
 * #NULL.
 *
 * @param message the message
 * @param signature the type signature or #NULL to unset
 * @returns #FALSE if no memory
 */
static dbus_bool_t
_dbus_message_set_signature (DBusMessage *message,
                             const char  *signature)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (signature == NULL ||
                            _dbus_check_is_valid_signature (signature));
  /* can't delete the signature if you have a message body */
  _dbus_return_val_if_fail (_dbus_string_get_length (&message->body) == 0 ||
                            signature != NULL);

  return set_or_delete_string_field (message,
                                     DBUS_HEADER_FIELD_SIGNATURE,
                                     DBUS_TYPE_SIGNATURE,
                                     signature);
}
#endif

/* Message Cache
 *
 * We cache some DBusMessage to reduce the overhead of allocating
 * them.  In my profiling this consistently made about an 8%
 * difference.  It avoids the malloc for the message, the malloc for
 * the slot list, the malloc for the header string and body string,
 * and the associated free() calls. It does introduce another global
 * lock which could be a performance issue in certain cases.
 *
 * For the echo client/server the round trip time goes from around
 * .000077 to .000069 with the message cache on my laptop. The sysprof
 * change is as follows (numbers are cumulative percentage):
 *
 *  with message cache implemented as array as it is now (0.000069 per):
 *    new_empty_header           1.46
 *      mutex_lock               0.56    # i.e. _DBUS_LOCK(message_cache)
 *      mutex_unlock             0.25
 *      self                     0.41
 *    unref                      2.24
 *      self                     0.68
 *      list_clear               0.43
 *      mutex_lock               0.33    # i.e. _DBUS_LOCK(message_cache)
 *      mutex_unlock             0.25
 *
 *  with message cache implemented as list (0.000070 per roundtrip):
 *    new_empty_header           2.72
 *      list_pop_first           1.88
 *    unref                      3.3
 *      list_prepend             1.63
 *
 * without cache (0.000077 per roundtrip):
 *    new_empty_header           6.7
 *      string_init_preallocated 3.43
 *        dbus_malloc            2.43
 *      dbus_malloc0             2.59
 *
 *    unref                      4.02
 *      string_free              1.82
 *        dbus_free              1.63
 *      dbus_free                0.71
 *
 * If you implement the message_cache with a list, the primary reason
 * it's slower is that you add another thread lock (on the DBusList
 * mempool).
 */

/** Avoid caching huge messages */
#define MAX_MESSAGE_SIZE_TO_CACHE 10 * _DBUS_ONE_KILOBYTE

/** Avoid caching too many messages */
#define MAX_MESSAGE_CACHE_SIZE    5

_DBUS_DEFINE_GLOBAL_LOCK (message_cache);
static DBusMessage *message_cache[MAX_MESSAGE_CACHE_SIZE];
static int message_cache_count = 0;
static dbus_bool_t message_cache_shutdown_registered = FALSE;

static void
dbus_message_cache_shutdown (void *data)
{
  int i;

  _DBUS_LOCK (message_cache);

  i = 0;
  while (i < MAX_MESSAGE_CACHE_SIZE)
    {
      if (message_cache[i])
        dbus_message_finalize (message_cache[i]);

      ++i;
    }

  message_cache_count = 0;
  message_cache_shutdown_registered = FALSE;

  _DBUS_UNLOCK (message_cache);
}

/**
 * Tries to get a message from the message cache.  The retrieved
 * message will have junk in it, so it still needs to be cleared out
 * in dbus_message_new_empty_header()
 *
 * @returns the message, or #NULL if none cached
 */
static DBusMessage*
dbus_message_get_cached (void)
{
  DBusMessage *message;
  int i;

  message = NULL;

  _DBUS_LOCK (message_cache);

  _dbus_assert (message_cache_count >= 0);

  if (message_cache_count == 0)
    {
      _DBUS_UNLOCK (message_cache);
      return NULL;
    }

  /* This is not necessarily true unless count > 0, and
   * message_cache is uninitialized until the shutdown is
   * registered
   */
  _dbus_assert (message_cache_shutdown_registered);

  i = 0;
  while (i < MAX_MESSAGE_CACHE_SIZE)
    {
      if (message_cache[i])
        {
          message = message_cache[i];
          message_cache[i] = NULL;
          message_cache_count -= 1;
          break;
        }
      ++i;
    }
  _dbus_assert (message_cache_count >= 0);
  _dbus_assert (i < MAX_MESSAGE_CACHE_SIZE);
  _dbus_assert (message != NULL);

  _DBUS_UNLOCK (message_cache);

  _dbus_assert (message->refcount.value == 0);
  _dbus_assert (message->size_counters == NULL);

  return message;
}

static void
free_size_counter (void *element,
                   void *data)
{
  DBusCounter *counter = element;
  DBusMessage *message = data;

  _dbus_counter_adjust (counter, - message->size_counter_delta);

  _dbus_counter_unref (counter);
}

/**
 * Tries to cache a message, otherwise finalize it.
 *
 * @param message the message
 */
static void
dbus_message_cache_or_finalize (DBusMessage *message)
{
  dbus_bool_t was_cached;
  int i;
  
  _dbus_assert (message->refcount.value == 0);

  /* This calls application code and has to be done first thing
   * without holding the lock
   */
  _dbus_data_slot_list_clear (&message->slot_list);

  _dbus_list_foreach (&message->size_counters,
                      free_size_counter, message);
  _dbus_list_clear (&message->size_counters);

  was_cached = FALSE;

  _DBUS_LOCK (message_cache);

  if (!message_cache_shutdown_registered)
    {
      _dbus_assert (message_cache_count == 0);

      if (!_dbus_register_shutdown_func (dbus_message_cache_shutdown, NULL))
        goto out;

      i = 0;
      while (i < MAX_MESSAGE_CACHE_SIZE)
        {
          message_cache[i] = NULL;
          ++i;
        }

      message_cache_shutdown_registered = TRUE;
    }

  _dbus_assert (message_cache_count >= 0);

  if ((_dbus_string_get_length (&message->header.data) +
       _dbus_string_get_length (&message->body)) >
      MAX_MESSAGE_SIZE_TO_CACHE)
    goto out;

  if (message_cache_count >= MAX_MESSAGE_CACHE_SIZE)
    goto out;

  /* Find empty slot */
  i = 0;
  while (message_cache[i] != NULL)
    ++i;

  _dbus_assert (i < MAX_MESSAGE_CACHE_SIZE);

  _dbus_assert (message_cache[i] == NULL);
  message_cache[i] = message;
  message_cache_count += 1;
  was_cached = TRUE;
#ifndef DBUS_DISABLE_CHECKS
  message->in_cache = TRUE;
#endif

 out:
  _DBUS_UNLOCK (message_cache);

  _dbus_assert (message->refcount.value == 0);
  
  if (!was_cached)
    dbus_message_finalize (message);
}

/** @} */

/**
 * @defgroup DBusMessage DBusMessage
 * @ingroup  DBus
 * @brief Message to be sent or received over a DBusConnection.
 *
 * A DBusMessage is the most basic unit of communication over a
 * DBusConnection. A DBusConnection represents a stream of messages
 * received from a remote application, and a stream of messages
 * sent to a remote application.
 *
 * @{
 */

/**
 * @typedef DBusMessage
 *
 * Opaque data type representing a message received from or to be
 * sent to another application.
 */

/**
 * Returns the serial of a message or 0 if none has been specified.
 * The message's serial number is provided by the application sending
 * the message and is used to identify replies to this message.  All
 * messages received on a connection will have a serial, but messages
 * you haven't sent yet may return 0.
 *
 * @param message the message
 * @returns the client serial
 */
dbus_uint32_t
dbus_message_get_serial (DBusMessage *message)
{
  _dbus_return_val_if_fail (message != NULL, 0);

  return _dbus_header_get_serial (&message->header);
}

/**
 * Sets the reply serial of a message (the client serial
 * of the message this is a reply to).
 *
 * @param message the message
 * @param reply_serial the client serial
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_reply_serial (DBusMessage   *message,
                               dbus_uint32_t  reply_serial)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (reply_serial != 0, FALSE); /* 0 is invalid */

  return _dbus_header_set_field_basic (&message->header,
                                       DBUS_HEADER_FIELD_REPLY_SERIAL,
                                       DBUS_TYPE_UINT32,
                                       &reply_serial);
}

/**
 * Returns the serial that the message is a reply to or 0 if none.
 *
 * @param message the message
 * @returns the reply serial
 */
dbus_uint32_t
dbus_message_get_reply_serial  (DBusMessage *message)
{
  dbus_uint32_t v_UINT32;

  _dbus_return_val_if_fail (message != NULL, 0);

  if (_dbus_header_get_field_basic (&message->header,
                                    DBUS_HEADER_FIELD_REPLY_SERIAL,
                                    DBUS_TYPE_UINT32,
                                    &v_UINT32))
    return v_UINT32;
  else
    return 0;
}

static void
dbus_message_finalize (DBusMessage *message)
{
  _dbus_assert (message->refcount.value == 0);

  /* This calls application callbacks! */
  _dbus_data_slot_list_free (&message->slot_list);

  _dbus_list_foreach (&message->size_counters,
                      free_size_counter, message);
  _dbus_list_clear (&message->size_counters);

  _dbus_header_free (&message->header);
  _dbus_string_free (&message->body);

  _dbus_assert (message->refcount.value == 0);
  
  dbus_free (message);
}

static DBusMessage*
dbus_message_new_empty_header (void)
{
  DBusMessage *message;
  dbus_bool_t from_cache;

  message = dbus_message_get_cached ();

  if (message != NULL)
    {
      from_cache = TRUE;
    }
  else
    {
      from_cache = FALSE;
      message = dbus_new (DBusMessage, 1);
      if (message == NULL)
        return NULL;
#ifndef DBUS_DISABLE_CHECKS
      message->generation = _dbus_current_generation;
#endif
    }
  
  message->refcount.value = 1;
  message->byte_order = DBUS_COMPILER_BYTE_ORDER;
  message->locked = FALSE;
#ifndef DBUS_DISABLE_CHECKS
  message->in_cache = FALSE;
#endif
  message->size_counters = NULL;
  message->size_counter_delta = 0;
  message->changed_stamp = 0;

  if (!from_cache)
    _dbus_data_slot_list_init (&message->slot_list);

  if (from_cache)
    {
      _dbus_header_reinit (&message->header, message->byte_order);
      _dbus_string_set_length (&message->body, 0);
    }
  else
    {
      if (!_dbus_header_init (&message->header, message->byte_order))
        {
          dbus_free (message);
          return NULL;
        }

      if (!_dbus_string_init_preallocated (&message->body, 32))
        {
          _dbus_header_free (&message->header);
          dbus_free (message);
          return NULL;
        }
    }

  return message;
}

/**
 * Constructs a new message of the given message type.
 * Types include #DBUS_MESSAGE_TYPE_METHOD_CALL,
 * #DBUS_MESSAGE_TYPE_SIGNAL, and so forth.
 *
 * @param message_type type of message
 * @returns new message or #NULL If no memory
 */
DBusMessage*
dbus_message_new (int message_type)
{
  DBusMessage *message;

  _dbus_return_val_if_fail (message_type != DBUS_MESSAGE_TYPE_INVALID, NULL);

  message = dbus_message_new_empty_header ();
  if (message == NULL)
    return NULL;

  if (!_dbus_header_create (&message->header,
                            message_type,
                            NULL, NULL, NULL, NULL, NULL))
    {
      dbus_message_unref (message);
      return NULL;
    }

  return message;
}

/**
 * Constructs a new message to invoke a method on a remote
 * object. Returns #NULL if memory can't be allocated for the
 * message. The destination may be #NULL in which case no destination
 * is set; this is appropriate when using D-Bus in a peer-to-peer
 * context (no message bus). The interface may be #NULL, which means
 * that if multiple methods with the given name exist it is undefined
 * which one will be invoked.
  *
 * @param destination name that the message should be sent to or #NULL
 * @param path object path the message should be sent to
 * @param interface interface to invoke method on
 * @param method method to invoke
 *
 * @returns a new DBusMessage, free with dbus_message_unref()
 * @see dbus_message_unref()
 */
DBusMessage*
dbus_message_new_method_call (const char *destination,
                              const char *path,
                              const char *interface,
                              const char *method)
{
  DBusMessage *message;

  _dbus_return_val_if_fail (path != NULL, NULL);
  _dbus_return_val_if_fail (method != NULL, NULL);
  _dbus_return_val_if_fail (destination == NULL ||
                            _dbus_check_is_valid_bus_name (destination), NULL);
  _dbus_return_val_if_fail (_dbus_check_is_valid_path (path), NULL);
  _dbus_return_val_if_fail (interface == NULL ||
                            _dbus_check_is_valid_interface (interface), NULL);
  _dbus_return_val_if_fail (_dbus_check_is_valid_member (method), NULL);

  message = dbus_message_new_empty_header ();
  if (message == NULL)
    return NULL;

  if (!_dbus_header_create (&message->header,
                            DBUS_MESSAGE_TYPE_METHOD_CALL,
                            destination, path, interface, method, NULL))
    {
      dbus_message_unref (message);
      return NULL;
    }

  return message;
}

/**
 * Constructs a message that is a reply to a method call. Returns
 * #NULL if memory can't be allocated for the message.
 *
 * @param method_call the message which the created
 * message is a reply to.
 * @returns a new DBusMessage, free with dbus_message_unref()
 * @see dbus_message_new_method_call(), dbus_message_unref()
 */
DBusMessage*
dbus_message_new_method_return (DBusMessage *method_call)
{
  DBusMessage *message;
  const char *sender;

  _dbus_return_val_if_fail (method_call != NULL, NULL);

  sender = dbus_message_get_sender (method_call);

  /* sender is allowed to be null here in peer-to-peer case */

  message = dbus_message_new_empty_header ();
  if (message == NULL)
    return NULL;

  if (!_dbus_header_create (&message->header,
                            DBUS_MESSAGE_TYPE_METHOD_RETURN,
                            sender, NULL, NULL, NULL, NULL))
    {
      dbus_message_unref (message);
      return NULL;
    }

  dbus_message_set_no_reply (message, TRUE);

  if (!dbus_message_set_reply_serial (message,
                                      dbus_message_get_serial (method_call)))
    {
      dbus_message_unref (message);
      return NULL;
    }

  return message;
}

/**
 * Constructs a new message representing a signal emission. Returns
 * #NULL if memory can't be allocated for the message.  A signal is
 * identified by its originating interface, and the name of the
 * signal.
 *
 * @param path the path to the object emitting the signal
 * @param interface the interface the signal is emitted from
 * @param name name of the signal
 * @returns a new DBusMessage, free with dbus_message_unref()
 * @see dbus_message_unref()
 */
DBusMessage*
dbus_message_new_signal (const char *path,
                         const char *interface,
                         const char *name)
{
  DBusMessage *message;

  _dbus_return_val_if_fail (path != NULL, NULL);
  _dbus_return_val_if_fail (interface != NULL, NULL);
  _dbus_return_val_if_fail (name != NULL, NULL);
  _dbus_return_val_if_fail (_dbus_check_is_valid_path (path), NULL);
  _dbus_return_val_if_fail (_dbus_check_is_valid_interface (interface), NULL);
  _dbus_return_val_if_fail (_dbus_check_is_valid_member (name), NULL);

  message = dbus_message_new_empty_header ();
  if (message == NULL)
    return NULL;

  if (!_dbus_header_create (&message->header,
                            DBUS_MESSAGE_TYPE_SIGNAL,
                            NULL, path, interface, name, NULL))
    {
      dbus_message_unref (message);
      return NULL;
    }

  dbus_message_set_no_reply (message, TRUE);

  return message;
}

/**
 * Creates a new message that is an error reply to a certain message.
 * Error replies are possible in response to method calls primarily.
 *
 * @param reply_to the original message
 * @param error_name the error name
 * @param error_message the error message string or #NULL for none
 * @returns a new error message
 */
DBusMessage*
dbus_message_new_error (DBusMessage *reply_to,
                        const char  *error_name,
                        const char  *error_message)
{
  DBusMessage *message;
  const char *sender;
  DBusMessageIter iter;

  _dbus_return_val_if_fail (reply_to != NULL, NULL);
  _dbus_return_val_if_fail (error_name != NULL, NULL);
  _dbus_return_val_if_fail (_dbus_check_is_valid_error_name (error_name), NULL);

  sender = dbus_message_get_sender (reply_to);

  /* sender may be NULL for non-message-bus case or
   * when the message bus is dealing with an unregistered
   * connection.
   */
  message = dbus_message_new_empty_header ();
  if (message == NULL)
    return NULL;

  if (!_dbus_header_create (&message->header,
                            DBUS_MESSAGE_TYPE_ERROR,
                            sender, NULL, NULL, NULL, error_name))
    {
      dbus_message_unref (message);
      return NULL;
    }

  dbus_message_set_no_reply (message, TRUE);

  if (!dbus_message_set_reply_serial (message,
                                      dbus_message_get_serial (reply_to)))
    {
      dbus_message_unref (message);
      return NULL;
    }

  if (error_message != NULL)
    {
      dbus_message_iter_init_append (message, &iter);
      if (!dbus_message_iter_append_basic (&iter,
                                           DBUS_TYPE_STRING,
                                           &error_message))
        {
          dbus_message_unref (message);
          return NULL;
        }
    }

  return message;
}

/**
 * Creates a new message that is an error reply to a certain message.
 * Error replies are possible in response to method calls primarily.
 *
 * @param reply_to the original message
 * @param error_name the error name
 * @param error_format the error message format as with printf
 * @param ... format string arguments
 * @returns a new error message
 */
DBusMessage*
dbus_message_new_error_printf (DBusMessage *reply_to,
			       const char  *error_name,
			       const char  *error_format,
			       ...)
{
  va_list args;
  DBusString str;
  DBusMessage *message;

  _dbus_return_val_if_fail (reply_to != NULL, NULL);
  _dbus_return_val_if_fail (error_name != NULL, NULL);
  _dbus_return_val_if_fail (_dbus_check_is_valid_error_name (error_name), NULL);

  if (!_dbus_string_init (&str))
    return NULL;

  va_start (args, error_format);

  if (_dbus_string_append_printf_valist (&str, error_format, args))
    message = dbus_message_new_error (reply_to, error_name,
				      _dbus_string_get_const_data (&str));
  else
    message = NULL;

  _dbus_string_free (&str);

  va_end (args);

  return message;
}


/**
 * Creates a new message that is an exact replica of the message
 * specified, except that its refcount is set to 1, its message serial
 * is reset to 0, and if the original message was "locked" (in the
 * outgoing message queue and thus not modifiable) the new message
 * will not be locked.
 *
 * @param message the message.
 * @returns the new message.
 */
DBusMessage *
dbus_message_copy (const DBusMessage *message)
{
  DBusMessage *retval;

  _dbus_return_val_if_fail (message != NULL, NULL);

  retval = dbus_new0 (DBusMessage, 1);
  if (retval == NULL)
    return NULL;

  retval->refcount.value = 1;
  retval->byte_order = message->byte_order;
  retval->locked = FALSE;
#ifndef DBUS_DISABLE_CHECKS
  retval->generation = message->generation;
#endif

  if (!_dbus_header_copy (&message->header, &retval->header))
    {
      dbus_free (retval);
      return NULL;
    }

  if (!_dbus_string_init_preallocated (&retval->body,
                                       _dbus_string_get_length (&message->body)))
    {
      _dbus_header_free (&retval->header);
      dbus_free (retval);
      return NULL;
    }

  if (!_dbus_string_copy (&message->body, 0,
			  &retval->body, 0))
    goto failed_copy;

  return retval;

 failed_copy:
  _dbus_header_free (&retval->header);
  _dbus_string_free (&retval->body);
  dbus_free (retval);

  return NULL;
}


/**
 * Increments the reference count of a DBusMessage.
 *
 * @param message The message
 * @returns the message
 * @see dbus_message_unref
 */
DBusMessage *
dbus_message_ref (DBusMessage *message)
{
  dbus_int32_t old_refcount;

  _dbus_return_val_if_fail (message != NULL, NULL);
  _dbus_return_val_if_fail (message->generation == _dbus_current_generation, NULL);
  _dbus_return_val_if_fail (!message->in_cache, NULL);
  
  old_refcount = _dbus_atomic_inc (&message->refcount);
  _dbus_assert (old_refcount >= 1);

  return message;
}

/**
 * Decrements the reference count of a DBusMessage.
 *
 * @param message The message
 * @see dbus_message_ref
 */
void
dbus_message_unref (DBusMessage *message)
{
 dbus_int32_t old_refcount;

  _dbus_return_if_fail (message != NULL);
  _dbus_return_if_fail (message->generation == _dbus_current_generation);
  _dbus_return_if_fail (!message->in_cache);

  old_refcount = _dbus_atomic_dec (&message->refcount);

  _dbus_assert (old_refcount >= 0);

  if (old_refcount == 1)
    {
      /* Calls application callbacks! */
      dbus_message_cache_or_finalize (message);
    }
}

/**
 * Gets the type of a message. Types include
 * #DBUS_MESSAGE_TYPE_METHOD_CALL, #DBUS_MESSAGE_TYPE_METHOD_RETURN,
 * #DBUS_MESSAGE_TYPE_ERROR, #DBUS_MESSAGE_TYPE_SIGNAL, but other
 * types are allowed and all code must silently ignore messages of
 * unknown type. #DBUS_MESSAGE_TYPE_INVALID will never be returned,
 * however.
 *
 * @param message the message
 * @returns the type of the message
 */
int
dbus_message_get_type (DBusMessage *message)
{
  _dbus_return_val_if_fail (message != NULL, DBUS_MESSAGE_TYPE_INVALID);

  return _dbus_header_get_message_type (&message->header);
}

/**
 * Appends fields to a message given a variable argument list. The
 * variable argument list should contain the type of each argument
 * followed by the value to append. Appendable types are basic types,
 * and arrays of fixed-length basic types. To append variable-length
 * basic types, or any more complex value, you have to use an iterator
 * rather than this function.
 *
 * To append a basic type, specify its type code followed by the
 * address of the value. For example:
 *
 * @code
 *
 * dbus_int32_t v_INT32 = 42;
 * const char *v_STRING = "Hello World";
 * DBUS_TYPE_INT32, &v_INT32,
 * DBUS_TYPE_STRING, &v_STRING,
 * @endcode
 *
 * To append an array of fixed-length basic types, pass in the
 * DBUS_TYPE_ARRAY typecode, the element typecode, the address of
 * the array pointer, and a 32-bit integer giving the number of
 * elements in the array. So for example:
 * @code
 * const dbus_int32_t array[] = { 1, 2, 3 };
 * const dbus_int32_t *v_ARRAY = array;
 * DBUS_TYPE_ARRAY, DBUS_TYPE_INT32, &v_ARRAY, 3
 * @endcode
 *
 * @warning in C, given "int array[]", "&array == array" (the
 * comp.lang.c FAQ says otherwise, but gcc and the FAQ don't agree).
 * So if you're using an array instead of a pointer you have to create
 * a pointer variable, assign the array to it, then take the address
 * of the pointer variable. For strings it works to write
 * const char *array = "Hello" and then use &array though.
 *
 * The last argument to this function must be #DBUS_TYPE_INVALID,
 * marking the end of the argument list.
 *
 * String/signature/path arrays should be passed in as "const char***
 * address_of_array" and "int n_elements"
 *
 * @todo support DBUS_TYPE_STRUCT and DBUS_TYPE_VARIANT and complex arrays
 *
 * @todo If this fails due to lack of memory, the message is hosed and
 * you have to start over building the whole message.
 *
 * @param message the message
 * @param first_arg_type type of the first argument
 * @param ... value of first argument, list of additional type-value pairs
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_args (DBusMessage *message,
			  int          first_arg_type,
			  ...)
{
  dbus_bool_t retval;
  va_list var_args;

  _dbus_return_val_if_fail (message != NULL, FALSE);

  va_start (var_args, first_arg_type);
  retval = dbus_message_append_args_valist (message,
					    first_arg_type,
					    var_args);
  va_end (var_args);

  return retval;
}

/**
 * This function takes a va_list for use by language bindings.
 * It's otherwise the same as dbus_message_append_args().
 *
 * @todo for now, if this function fails due to OOM it will leave
 * the message half-written and you have to discard the message
 * and start over.
 *
 * @see dbus_message_append_args.
 * @param message the message
 * @param first_arg_type type of first argument
 * @param var_args value of first argument, then list of type/value pairs
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_args_valist (DBusMessage *message,
				 int          first_arg_type,
				 va_list      var_args)
{
  int type;
  DBusMessageIter iter;

  _dbus_return_val_if_fail (message != NULL, FALSE);

  type = first_arg_type;

  dbus_message_iter_init_append (message, &iter);

  while (type != DBUS_TYPE_INVALID)
    {
      if (dbus_type_is_basic (type))
        {
          const DBusBasicValue *value;
          value = va_arg (var_args, const DBusBasicValue*);

          if (!dbus_message_iter_append_basic (&iter,
                                               type,
                                               value))
            goto failed;
        }
      else if (type == DBUS_TYPE_ARRAY)
        {
          int element_type;
          DBusMessageIter array;
          char buf[2];

          element_type = va_arg (var_args, int);
              
          buf[0] = element_type;
          buf[1] = '\0';
          if (!dbus_message_iter_open_container (&iter,
                                                 DBUS_TYPE_ARRAY,
                                                 buf,
                                                 &array))
            goto failed;
          
          if (dbus_type_is_fixed (element_type))
            {
              const DBusBasicValue **value;
              int n_elements;

              value = va_arg (var_args, const DBusBasicValue**);
              n_elements = va_arg (var_args, int);
              
              if (!dbus_message_iter_append_fixed_array (&array,
                                                         element_type,
                                                         value,
                                                         n_elements))
                goto failed;
            }
          else if (element_type == DBUS_TYPE_STRING ||
                   element_type == DBUS_TYPE_SIGNATURE ||
                   element_type == DBUS_TYPE_OBJECT_PATH)
            {
              const char ***value_p;
              const char **value;
              int n_elements;
              int i;
              
              value_p = va_arg (var_args, const char***);
              n_elements = va_arg (var_args, int);

              value = *value_p;
              
              i = 0;
              while (i < n_elements)
                {
                  if (!dbus_message_iter_append_basic (&array,
                                                       element_type,
                                                       &value[i]))
                    goto failed;
                  ++i;
                }
            }
          else
            {
              _dbus_warn ("arrays of %s can't be appended with %s for now\n",
                          _dbus_type_to_string (element_type),
                          _DBUS_FUNCTION_NAME);
              goto failed;
            }

          if (!dbus_message_iter_close_container (&iter, &array))
            goto failed;
        }
#ifndef DBUS_DISABLE_CHECKS
      else
        {
          _dbus_warn ("type %s isn't supported yet in %s\n",
                      _dbus_type_to_string (type), _DBUS_FUNCTION_NAME);
          goto failed;
        }
#endif

      type = va_arg (var_args, int);
    }

  return TRUE;

 failed:
  return FALSE;
}

/**
 * Gets arguments from a message given a variable argument list.  The
 * supported types include those supported by
 * dbus_message_append_args(); that is, basic types and arrays of
 * fixed-length basic types.  The arguments are the same as they would
 * be for dbus_message_iter_get_basic() or
 * dbus_message_iter_get_fixed_array().
 *
 * In addition to those types, arrays of string, object path, and
 * signature are supported; but these are returned as allocated memory
 * and must be freed with dbus_free_string_array(), while the other
 * types are returned as const references. To get a string array
 * pass in "char ***array_location" and "int *n_elements"
 *
 * The variable argument list should contain the type of the argument
 * followed by a pointer to where the value should be stored. The list
 * is terminated with #DBUS_TYPE_INVALID.
 *
 * The returned values are constant; do not free them. They point
 * into the #DBusMessage.
 *
 * If the requested arguments are not present, or do not have the
 * requested types, then an error will be set.
 *
 * @todo support DBUS_TYPE_STRUCT and DBUS_TYPE_VARIANT and complex arrays
 *
 * @param message the message
 * @param error error to be filled in on failure
 * @param first_arg_type the first argument type
 * @param ... location for first argument value, then list of type-location pairs
 * @returns #FALSE if the error was set
 */
dbus_bool_t
dbus_message_get_args (DBusMessage     *message,
                       DBusError       *error,
		       int              first_arg_type,
		       ...)
{
  dbus_bool_t retval;
  va_list var_args;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_error_is_set (error, FALSE);

  va_start (var_args, first_arg_type);
  retval = dbus_message_get_args_valist (message, error, first_arg_type, var_args);
  va_end (var_args);

  return retval;
}

/**
 * This function takes a va_list for use by language bindings. It is
 * otherwise the same as dbus_message_get_args().
 *
 * @see dbus_message_get_args
 * @param message the message
 * @param error error to be filled in
 * @param first_arg_type type of the first argument
 * @param var_args return location for first argument, followed by list of type/location pairs
 * @returns #FALSE if error was set
 */
dbus_bool_t
dbus_message_get_args_valist (DBusMessage     *message,
                              DBusError       *error,
			      int              first_arg_type,
			      va_list          var_args)
{
  DBusMessageIter iter;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_error_is_set (error, FALSE);

  dbus_message_iter_init (message, &iter);
  return _dbus_message_iter_get_args_valist (&iter, error, first_arg_type, var_args);
}

static void
_dbus_message_iter_init_common (DBusMessage         *message,
                                DBusMessageRealIter *real,
                                int                  iter_type)
{
  _dbus_assert (sizeof (DBusMessageRealIter) <= sizeof (DBusMessageIter));

  /* Since the iterator will read or write who-knows-what from the
   * message, we need to get in the right byte order
   */
  ensure_byte_order (message);
  
  real->message = message;
  real->changed_stamp = message->changed_stamp;
  real->iter_type = iter_type;
  real->sig_refcount = 0;
}

/**
 * Initializes a #DBusMessageIter for reading the arguments of the
 * message passed in.
 *
 * @param message the message
 * @param iter pointer to an iterator to initialize
 * @returns #FALSE if the message has no arguments
 */
dbus_bool_t
dbus_message_iter_init (DBusMessage     *message,
			DBusMessageIter *iter)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  const DBusString *type_str;
  int type_pos;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (iter != NULL, FALSE);

  get_const_signature (&message->header, &type_str, &type_pos);

  _dbus_message_iter_init_common (message, real,
                                  DBUS_MESSAGE_ITER_TYPE_READER);

  _dbus_type_reader_init (&real->u.reader,
                          message->byte_order,
                          type_str, type_pos,
                          &message->body,
                          0);

  return _dbus_type_reader_get_current_type (&real->u.reader) != DBUS_TYPE_INVALID;
}

#ifndef DBUS_DISABLE_CHECKS
static dbus_bool_t
_dbus_message_iter_check (DBusMessageRealIter *iter)
{
  if (iter == NULL)
    {
      _dbus_warn ("dbus message iterator is NULL\n");
      return FALSE;
    }

  if (iter->iter_type == DBUS_MESSAGE_ITER_TYPE_READER)
    {
      if (iter->u.reader.byte_order != iter->message->byte_order)
        {
          _dbus_warn ("dbus message changed byte order since iterator was created\n");
          return FALSE;
        }
      /* because we swap the message into compiler order when you init an iter */
      _dbus_assert (iter->u.reader.byte_order == DBUS_COMPILER_BYTE_ORDER);
    }
  else if (iter->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER)
    {
      if (iter->u.writer.byte_order != iter->message->byte_order)
        {
          _dbus_warn ("dbus message changed byte order since append iterator was created\n");
          return FALSE;
        }
      /* because we swap the message into compiler order when you init an iter */
      _dbus_assert (iter->u.writer.byte_order == DBUS_COMPILER_BYTE_ORDER);
    }
  else
    {
      _dbus_warn ("dbus message iterator looks uninitialized or corrupted\n");
      return FALSE;
    }

  if (iter->changed_stamp != iter->message->changed_stamp)
    {
      _dbus_warn ("dbus message iterator invalid because the message has been modified (or perhaps the iterator is just uninitialized)\n");
      return FALSE;
    }

  return TRUE;
}
#endif /* DBUS_DISABLE_CHECKS */

/**
 * Checks if an iterator has any more fields.
 *
 * @param iter the message iter
 * @returns #TRUE if there are more fields
 * following
 */
dbus_bool_t
dbus_message_iter_has_next (DBusMessageIter *iter)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_val_if_fail (_dbus_message_iter_check (real), FALSE);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_READER, FALSE);

  return _dbus_type_reader_has_next (&real->u.reader);
}

/**
 * Moves the iterator to the next field, if any. If there's no next
 * field, returns #FALSE. If the iterator moves forward, returns
 * #TRUE.
 *
 * @param iter the message iter
 * @returns #TRUE if the iterator was moved to the next field
 */
dbus_bool_t
dbus_message_iter_next (DBusMessageIter *iter)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_val_if_fail (_dbus_message_iter_check (real), FALSE);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_READER, FALSE);

  return _dbus_type_reader_next (&real->u.reader);
}

/**
 * Returns the argument type of the argument that the message iterator
 * points to. If the iterator is at the end of the message, returns
 * #DBUS_TYPE_INVALID. You can thus write a loop as follows:
 *
 * @code
 * dbus_message_iter_init (&iter);
 * while ((current_type = dbus_message_iter_get_arg_type (&iter)) != DBUS_TYPE_INVALID)
 *   dbus_message_iter_next (&iter);
 * @endcode
 *
 * @param iter the message iter
 * @returns the argument type
 */
int
dbus_message_iter_get_arg_type (DBusMessageIter *iter)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_val_if_fail (_dbus_message_iter_check (real), DBUS_TYPE_INVALID);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_READER, FALSE);

  return _dbus_type_reader_get_current_type (&real->u.reader);
}

/**
 * Returns the element type of the array that the message iterator
 * points to. Note that you need to check that the iterator points to
 * an array prior to using this function.
 *
 * @param iter the message iter
 * @returns the array element type
 */
int
dbus_message_iter_get_element_type (DBusMessageIter *iter)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_val_if_fail (_dbus_message_iter_check (real), DBUS_TYPE_INVALID);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_READER, DBUS_TYPE_INVALID);
  _dbus_return_val_if_fail (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_ARRAY, DBUS_TYPE_INVALID);

  return _dbus_type_reader_get_element_type (&real->u.reader);
}

/**
 * Recurses into a container value when reading values from a message,
 * initializing a sub-iterator to use for traversing the child values
 * of the container.
 *
 * Note that this recurses into a value, not a type, so you can only
 * recurse if the value exists. The main implication of this is that
 * if you have for example an empty array of array of int32, you can
 * recurse into the outermost array, but it will have no values, so
 * you won't be able to recurse further. There's no array of int32 to
 * recurse into.
 *
 * @param iter the message iterator
 * @param sub the sub-iterator to initialize
 */
void
dbus_message_iter_recurse (DBusMessageIter  *iter,
                           DBusMessageIter  *sub)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  DBusMessageRealIter *real_sub = (DBusMessageRealIter *)sub;

  _dbus_return_if_fail (_dbus_message_iter_check (real));
  _dbus_return_if_fail (sub != NULL);

  *real_sub = *real;
  _dbus_type_reader_recurse (&real->u.reader, &real_sub->u.reader);
}

/**
 * Returns the current signature of a message iterator.  This
 * is useful primarily for dealing with variants; one can
 * recurse into a variant and determine the signature of
 * the variant's value.
 *
 * @param iter the message iterator
 * @returns the contained signature, or NULL if out of memory
 */
char *
dbus_message_iter_get_signature (DBusMessageIter *iter)
{
  const DBusString *sig;
  DBusString retstr;
  char *ret;
  int start, len;
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_val_if_fail (_dbus_message_iter_check (real), NULL);

  if (!_dbus_string_init (&retstr))
    return NULL;

  _dbus_type_reader_get_signature (&real->u.reader, &sig,
				   &start, &len);
  if (!_dbus_string_append_len (&retstr,
				_dbus_string_get_const_data (sig) + start,
				len))
    return NULL;
  if (!_dbus_string_steal_data (&retstr, &ret))
    return NULL;
  _dbus_string_free (&retstr);
  return ret;
}

/**
 * Reads a basic-typed value from the message iterator.
 * Basic types are the non-containers such as integer and string.
 *
 * The value argument should be the address of a location to store
 * the returned value. So for int32 it should be a "dbus_int32_t*"
 * and for string a "const char**". The returned value is
 * by reference and should not be freed.
 *
 * All returned values are guaranteed to fit in 8 bytes. So you can
 * write code like this:
 *
 * @code
 * #ifdef DBUS_HAVE_INT64
 * dbus_uint64_t value;
 * int type;
 * dbus_message_iter_get_basic (&read_iter, &value);
 * type = dbus_message_iter_get_arg_type (&read_iter);
 * dbus_message_iter_append_basic (&write_iter, type, &value);
 * #endif
 * @endcode
 *
 * To avoid the #DBUS_HAVE_INT64 conditional, create a struct or
 * something that occupies at least 8 bytes, e.g. you could use a
 * struct with two int32 values in it. dbus_uint64_t is just one
 * example of a type that's large enough to hold any possible value.
 *
 * Be sure you have somehow checked that
 * dbus_message_iter_get_arg_type() matches the type you are
 * expecting, or you'll crash when you try to use an integer as a
 * string or something.
 *
 * @param iter the iterator
 * @param value location to store the value
 */
void
dbus_message_iter_get_basic (DBusMessageIter  *iter,
                             void             *value)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_if_fail (_dbus_message_iter_check (real));
  _dbus_return_if_fail (value != NULL);

  _dbus_type_reader_read_basic (&real->u.reader,
                                value);
}

/**
 * Returns the number of elements in the array;
 *
 * @param iter the iterator
 * @returns the number of elements in the array
 */
int
dbus_message_iter_get_array_len (DBusMessageIter *iter)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_val_if_fail (_dbus_message_iter_check (real), 0);

  return _dbus_type_reader_get_array_length (&real->u.reader);
}

/**
 * Reads a block of fixed-length values from the message iterator.
 * Fixed-length values are those basic types that are not string-like,
 * such as integers, bool, double. The block read will be from the
 * current position in the array until the end of the array.
 *
 * This function should only be used if #dbus_type_is_fixed returns
 * #TRUE for the element type.
 *
 * The value argument should be the address of a location to store the
 * returned array. So for int32 it should be a "const dbus_int32_t**"
 * The returned value is by reference and should not be freed.
 *
 * @param iter the iterator
 * @param value location to store the block
 * @param n_elements number of elements in the block
 */
void
dbus_message_iter_get_fixed_array (DBusMessageIter  *iter,
                                   void             *value,
                                   int              *n_elements)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  int subtype = _dbus_type_reader_get_current_type(&real->u.reader);

  _dbus_return_if_fail (_dbus_message_iter_check (real));
  _dbus_return_if_fail (value != NULL);
  _dbus_return_if_fail ((subtype == DBUS_TYPE_INVALID) ||
                         dbus_type_is_fixed (subtype));

  _dbus_type_reader_read_fixed_multi (&real->u.reader,
                                      value, n_elements);
}

/**
 * This function takes a va_list for use by language bindings and is
 * otherwise the same as dbus_message_iter_get_args().
 * dbus_message_get_args() is the place to go for complete
 * documentation.
 *
 * @see dbus_message_get_args
 * @param iter the message iter
 * @param error error to be filled in
 * @param first_arg_type type of the first argument
 * @param var_args return location for first argument, followed by list of type/location pairs
 * @returns #FALSE if error was set
 */
dbus_bool_t
_dbus_message_iter_get_args_valist (DBusMessageIter *iter,
                                    DBusError       *error,
                                    int              first_arg_type,
                                    va_list          var_args)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  int spec_type, msg_type, i;
  dbus_bool_t retval;

  _dbus_assert (_dbus_message_iter_check (real));

  retval = FALSE;

  spec_type = first_arg_type;
  i = 0;

  while (spec_type != DBUS_TYPE_INVALID)
    {
      msg_type = dbus_message_iter_get_arg_type (iter);

      if (msg_type != spec_type)
	{
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Argument %d is specified to be of type \"%s\", but "
                          "is actually of type \"%s\"\n", i,
                          _dbus_type_to_string (spec_type),
                          _dbus_type_to_string (msg_type));

          goto out;
	}

      if (dbus_type_is_basic (spec_type))
        {
          DBusBasicValue *ptr;

          ptr = va_arg (var_args, DBusBasicValue*);

          _dbus_assert (ptr != NULL);

          _dbus_type_reader_read_basic (&real->u.reader,
                                        ptr);
        }
      else if (spec_type == DBUS_TYPE_ARRAY)
        {
          int element_type;
          int spec_element_type;
          const DBusBasicValue **ptr;
          int *n_elements_p;
          DBusTypeReader array;

          spec_element_type = va_arg (var_args, int);
          element_type = _dbus_type_reader_get_element_type (&real->u.reader);

          if (spec_element_type != element_type)
            {
              dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                              "Argument %d is specified to be an array of \"%s\", but "
                              "is actually an array of \"%s\"\n",
                              i,
                              _dbus_type_to_string (spec_element_type),
                              _dbus_type_to_string (element_type));

              goto out;
            }

          if (dbus_type_is_fixed (spec_element_type))
            {
              ptr = va_arg (var_args, const DBusBasicValue**);
              n_elements_p = va_arg (var_args, int*);

              _dbus_assert (ptr != NULL);
              _dbus_assert (n_elements_p != NULL);

              _dbus_type_reader_recurse (&real->u.reader, &array);

              _dbus_type_reader_read_fixed_multi (&array,
                                                  ptr, n_elements_p);
            }
          else if (spec_element_type == DBUS_TYPE_STRING ||
                   spec_element_type == DBUS_TYPE_SIGNATURE ||
                   spec_element_type == DBUS_TYPE_OBJECT_PATH)
            {
              char ***str_array_p;
              int n_elements;
              char **str_array;

              str_array_p = va_arg (var_args, char***);
              n_elements_p = va_arg (var_args, int*);

              _dbus_assert (str_array_p != NULL);
              _dbus_assert (n_elements_p != NULL);

              /* Count elements in the array */
              _dbus_type_reader_recurse (&real->u.reader, &array);

              n_elements = 0;
              while (_dbus_type_reader_get_current_type (&array) != DBUS_TYPE_INVALID)
                {
                  ++n_elements;
                  _dbus_type_reader_next (&array);
                }

              str_array = dbus_new0 (char*, n_elements + 1);
              if (str_array == NULL)
                {
                  _DBUS_SET_OOM (error);
                  goto out;
                }

              /* Now go through and dup each string */
              _dbus_type_reader_recurse (&real->u.reader, &array);

              i = 0;
              while (i < n_elements)
                {
                  const char *s;
                  _dbus_type_reader_read_basic (&array,
                                                &s);
                  
                  str_array[i] = _dbus_strdup (s);
                  if (str_array[i] == NULL)
                    {
                      dbus_free_string_array (str_array);
                      _DBUS_SET_OOM (error);
                      goto out;
                    }
                  
                  ++i;
                  
                  if (!_dbus_type_reader_next (&array))
                    _dbus_assert (i == n_elements);
                }

              _dbus_assert (_dbus_type_reader_get_current_type (&array) == DBUS_TYPE_INVALID);
              _dbus_assert (i == n_elements);
              _dbus_assert (str_array[i] == NULL);

              *str_array_p = str_array;
              *n_elements_p = n_elements;
            }
#ifndef DBUS_DISABLE_CHECKS
          else
            {
              _dbus_warn ("you can't read arrays of container types (struct, variant, array) with %s for now\n",
                          _DBUS_FUNCTION_NAME);
              goto out;
            }
#endif
        }
#ifndef DBUS_DISABLE_CHECKS
      else
        {
          _dbus_warn ("you can only read arrays and basic types with %s for now\n",
                      _DBUS_FUNCTION_NAME);
          goto out;
        }
#endif

      spec_type = va_arg (var_args, int);
      if (!_dbus_type_reader_next (&real->u.reader) && spec_type != DBUS_TYPE_INVALID)
        {
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Message has only %d arguments, but more were expected", i);
          goto out;
        }

      i++;
    }

  retval = TRUE;

 out:

  return retval;
}

/**
 * Initializes a #DBusMessageIter for appending arguments to the end
 * of a message.
 *
 * @todo If appending any of the arguments fails due to lack of
 * memory, generally the message is hosed and you have to start over
 * building the whole message.
 *
 * @param message the message
 * @param iter pointer to an iterator to initialize
 */
void
dbus_message_iter_init_append (DBusMessage     *message,
			       DBusMessageIter *iter)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;

  _dbus_return_if_fail (message != NULL);
  _dbus_return_if_fail (iter != NULL);

  _dbus_message_iter_init_common (message, real,
                                  DBUS_MESSAGE_ITER_TYPE_WRITER);

  /* We create the signature string and point iterators at it "on demand"
   * when a value is actually appended. That means that init() never fails
   * due to OOM.
   */
  _dbus_type_writer_init_types_delayed (&real->u.writer,
                                        message->byte_order,
                                        &message->body,
                                        _dbus_string_get_length (&message->body));
}

/**
 * Creates a temporary signature string containing the current
 * signature, stores it in the iterator, and points the iterator to
 * the end of it. Used any time we write to the message.
 *
 * @param real an iterator without a type_str
 * @returns #FALSE if no memory
 */
static dbus_bool_t
_dbus_message_iter_open_signature (DBusMessageRealIter *real)
{
  DBusString *str;
  const DBusString *current_sig;
  int current_sig_pos;

  _dbus_assert (real->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER);

  if (real->u.writer.type_str != NULL)
    {
      _dbus_assert (real->sig_refcount > 0);
      real->sig_refcount += 1;
      return TRUE;
    }

  str = dbus_new (DBusString, 1);
  if (str == NULL)
    return FALSE;

  if (!_dbus_header_get_field_raw (&real->message->header,
                                   DBUS_HEADER_FIELD_SIGNATURE,
                                   &current_sig, &current_sig_pos))
    current_sig = NULL;

  if (current_sig)
    {
      int current_len;

      current_len = _dbus_string_get_byte (current_sig, current_sig_pos);
      current_sig_pos += 1; /* move on to sig data */

      if (!_dbus_string_init_preallocated (str, current_len + 4))
        {
          dbus_free (str);
          return FALSE;
        }

      if (!_dbus_string_copy_len (current_sig, current_sig_pos, current_len,
                                  str, 0))
        {
          _dbus_string_free (str);
          dbus_free (str);
          return FALSE;
        }
    }
  else
    {
      if (!_dbus_string_init_preallocated (str, 4))
        {
          dbus_free (str);
          return FALSE;
        }
    }

  real->sig_refcount = 1;

  _dbus_type_writer_add_types (&real->u.writer,
                               str, _dbus_string_get_length (str));
  return TRUE;
}

/**
 * Sets the new signature as the message signature, frees the
 * signature string, and marks the iterator as not having a type_str
 * anymore. Frees the signature even if it fails, so you can't
 * really recover from failure. Kinda busted.
 *
 * @param real an iterator without a type_str
 * @returns #FALSE if no memory
 */
static dbus_bool_t
_dbus_message_iter_close_signature (DBusMessageRealIter *real)
{
  DBusString *str;
  const char *v_STRING;
  dbus_bool_t retval;

  _dbus_assert (real->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER);
  _dbus_assert (real->u.writer.type_str != NULL);
  _dbus_assert (real->sig_refcount > 0);

  real->sig_refcount -= 1;

  if (real->sig_refcount > 0)
    return TRUE;
  _dbus_assert (real->sig_refcount == 0);

  retval = TRUE;

  str = real->u.writer.type_str;

  v_STRING = _dbus_string_get_const_data (str);
  if (!_dbus_header_set_field_basic (&real->message->header,
                                     DBUS_HEADER_FIELD_SIGNATURE,
                                     DBUS_TYPE_SIGNATURE,
                                     &v_STRING))
    retval = FALSE;

  _dbus_type_writer_remove_types (&real->u.writer);
  _dbus_string_free (str);
  dbus_free (str);

  return retval;
}

#ifndef DBUS_DISABLE_CHECKS
static dbus_bool_t
_dbus_message_iter_append_check (DBusMessageRealIter *iter)
{
  if (!_dbus_message_iter_check (iter))
    return FALSE;

  if (iter->message->locked)
    {
      _dbus_warn ("dbus append iterator can't be used: message is locked (has already been sent)\n");
      return FALSE;
    }

  return TRUE;
}
#endif /* DBUS_DISABLE_CHECKS */

/**
 * Appends a basic-typed value to the message. The basic types are the
 * non-container types such as integer and string.
 *
 * The "value" argument should be the address of a basic-typed value.
 * So for string, const char**. For integer, dbus_int32_t*.
 *
 * @todo If this fails due to lack of memory, the message is hosed and
 * you have to start over building the whole message.
 *
 * @param iter the append iterator
 * @param type the type of the value
 * @param value the address of the value
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_iter_append_basic (DBusMessageIter *iter,
                                int              type,
                                const void      *value)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  dbus_bool_t ret;

  _dbus_return_val_if_fail (_dbus_message_iter_append_check (real), FALSE);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER, FALSE);
  _dbus_return_val_if_fail (dbus_type_is_basic (type), FALSE);
  _dbus_return_val_if_fail (value != NULL, FALSE);

  if (!_dbus_message_iter_open_signature (real))
    return FALSE;

  ret = _dbus_type_writer_write_basic (&real->u.writer, type, value);

  if (!_dbus_message_iter_close_signature (real))
    ret = FALSE;

  return ret;
}

/**
 * Appends a block of fixed-length values to an array. The
 * fixed-length types are all basic types that are not string-like. So
 * int32, double, bool, etc. You must call
 * dbus_message_iter_open_container() to open an array of values
 * before calling this function. You may call this function multiple
 * times (and intermixed with calls to
 * dbus_message_iter_append_basic()) for the same array.
 *
 * The "value" argument should be the address of the array.  So for
 * integer, "dbus_int32_t**" is expected for example.
 *
 * @warning in C, given "int array[]", "&array == array" (the
 * comp.lang.c FAQ says otherwise, but gcc and the FAQ don't agree).
 * So if you're using an array instead of a pointer you have to create
 * a pointer variable, assign the array to it, then take the address
 * of the pointer variable.
 * @code
 * const dbus_int32_t array[] = { 1, 2, 3 };
 * const dbus_int32_t *v_ARRAY = array;
 * if (!dbus_message_iter_append_fixed_array (&iter, DBUS_TYPE_INT32, &v_ARRAY, 3))
 *   fprintf (stderr, "No memory!\n");
 * @endcode
 * For strings it works to write const char *array = "Hello" and then
 * use &array though.
 *
 * @todo If this fails due to lack of memory, the message is hosed and
 * you have to start over building the whole message.
 *
 * @param iter the append iterator
 * @param element_type the type of the array elements
 * @param value the address of the array
 * @param n_elements the number of elements to append
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_iter_append_fixed_array (DBusMessageIter *iter,
                                      int              element_type,
                                      const void      *value,
                                      int              n_elements)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  dbus_bool_t ret;

  _dbus_return_val_if_fail (_dbus_message_iter_append_check (real), FALSE);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER, FALSE);
  _dbus_return_val_if_fail (dbus_type_is_fixed (element_type), FALSE);
  _dbus_return_val_if_fail (real->u.writer.container_type == DBUS_TYPE_ARRAY, FALSE);
  _dbus_return_val_if_fail (value != NULL, FALSE);
  _dbus_return_val_if_fail (n_elements >= 0, FALSE);
  _dbus_return_val_if_fail (n_elements <=
                            DBUS_MAXIMUM_ARRAY_LENGTH / _dbus_type_get_alignment (element_type),
                            FALSE);

  ret = _dbus_type_writer_write_fixed_multi (&real->u.writer, element_type, value, n_elements);

  return ret;
}

/**
 * Appends a container-typed value to the message; you are required to
 * append the contents of the container using the returned
 * sub-iterator, and then call
 * dbus_message_iter_close_container(). Container types are for
 * example struct, variant, and array. For variants, the
 * contained_signature should be the type of the single value inside
 * the variant. For structs and dict entries, contained_signature
 * should be #NULL; it will be set to whatever types you write into
 * the struct.  For arrays, contained_signature should be the type of
 * the array elements.
 *
 * @todo If this fails due to lack of memory, the message is hosed and
 * you have to start over building the whole message.
 *
 * @param iter the append iterator
 * @param type the type of the value
 * @param contained_signature the type of container contents
 * @param sub sub-iterator to initialize
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_iter_open_container (DBusMessageIter *iter,
                                  int              type,
                                  const char      *contained_signature,
                                  DBusMessageIter *sub)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  DBusMessageRealIter *real_sub = (DBusMessageRealIter *)sub;
  DBusString contained_str;

  _dbus_return_val_if_fail (_dbus_message_iter_append_check (real), FALSE);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER, FALSE);
  _dbus_return_val_if_fail (dbus_type_is_container (type), FALSE);
  _dbus_return_val_if_fail (sub != NULL, FALSE);
  _dbus_return_val_if_fail ((type == DBUS_TYPE_STRUCT &&
                             contained_signature == NULL) ||
                            (type == DBUS_TYPE_DICT_ENTRY &&
                             contained_signature == NULL) ||
                            contained_signature != NULL, FALSE);
  
#if 0
  /* FIXME this would fail if the contained_signature is a dict entry,
   * since dict entries are invalid signatures standalone (they must be in
   * an array)
   */
  _dbus_return_val_if_fail (contained_signature == NULL ||
                            _dbus_check_is_valid_signature (contained_signature));
#endif

  if (!_dbus_message_iter_open_signature (real))
    return FALSE;

  *real_sub = *real;

  if (contained_signature != NULL)
    {
      _dbus_string_init_const (&contained_str, contained_signature);

      return _dbus_type_writer_recurse (&real->u.writer,
                                        type,
                                        &contained_str, 0,
                                        &real_sub->u.writer);
    }
  else
    {
      return _dbus_type_writer_recurse (&real->u.writer,
                                        type,
                                        NULL, 0,
                                        &real_sub->u.writer);
    } 
}


/**
 * Closes a container-typed value appended to the message; may write
 * out more information to the message known only after the entire
 * container is written, and may free resources created by
 * dbus_message_iter_open_container().
 *
 * @todo If this fails due to lack of memory, the message is hosed and
 * you have to start over building the whole message.
 *
 * @param iter the append iterator
 * @param sub sub-iterator to close
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_iter_close_container (DBusMessageIter *iter,
                                   DBusMessageIter *sub)
{
  DBusMessageRealIter *real = (DBusMessageRealIter *)iter;
  DBusMessageRealIter *real_sub = (DBusMessageRealIter *)sub;
  dbus_bool_t ret;

  _dbus_return_val_if_fail (_dbus_message_iter_append_check (real), FALSE);
  _dbus_return_val_if_fail (real->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER, FALSE);
  _dbus_return_val_if_fail (_dbus_message_iter_append_check (real_sub), FALSE);
  _dbus_return_val_if_fail (real_sub->iter_type == DBUS_MESSAGE_ITER_TYPE_WRITER, FALSE);

  ret = _dbus_type_writer_unrecurse (&real->u.writer,
                                     &real_sub->u.writer);

  if (!_dbus_message_iter_close_signature (real))
    ret = FALSE;

  return ret;
}

/**
 * Sets a flag indicating that the message does not want a reply; if
 * this flag is set, the other end of the connection may (but is not
 * required to) optimize by not sending method return or error
 * replies. If this flag is set, there is no way to know whether the
 * message successfully arrived at the remote end. Normally you know a
 * message was received when you receive the reply to it.
 *
 * @param message the message
 * @param no_reply #TRUE if no reply is desired
 */
void
dbus_message_set_no_reply (DBusMessage *message,
                           dbus_bool_t  no_reply)
{
  _dbus_return_if_fail (message != NULL);
  _dbus_return_if_fail (!message->locked);

  _dbus_header_toggle_flag (&message->header,
                            DBUS_HEADER_FLAG_NO_REPLY_EXPECTED,
                            no_reply);
}

/**
 * Returns #TRUE if the message does not expect
 * a reply.
 *
 * @param message the message
 * @returns #TRUE if the message sender isn't waiting for a reply
 */
dbus_bool_t
dbus_message_get_no_reply (DBusMessage *message)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);

  return _dbus_header_get_flag (&message->header,
                                DBUS_HEADER_FLAG_NO_REPLY_EXPECTED);
}

/**
 * Sets a flag indicating that an owner for the destination name will
 * be automatically started before the message is delivered. When this
 * flag is set, the message is held until a name owner finishes
 * starting up, or fails to start up. In case of failure, the reply
 * will be an error.
 *
 * @param message the message
 * @param auto_start #TRUE if auto-starting is desired
 */
void
dbus_message_set_auto_start (DBusMessage *message,
                             dbus_bool_t  auto_start)
{
  _dbus_return_if_fail (message != NULL);
  _dbus_return_if_fail (!message->locked);

  _dbus_header_toggle_flag (&message->header,
                            DBUS_HEADER_FLAG_NO_AUTO_START,
                            !auto_start);
}

/**
 * Returns #TRUE if the message will cause an owner for
 * destination name to be auto-started.
 *
 * @param message the message
 * @returns #TRUE if the message will use auto-start
 */
dbus_bool_t
dbus_message_get_auto_start (DBusMessage *message)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);

  return !_dbus_header_get_flag (&message->header,
                                 DBUS_HEADER_FLAG_NO_AUTO_START);
}


/**
 * Sets the object path this message is being sent to (for
 * DBUS_MESSAGE_TYPE_METHOD_CALL) or the one a signal is being
 * emitted from (for DBUS_MESSAGE_TYPE_SIGNAL).
 *
 * @param message the message
 * @param object_path the path or #NULL to unset
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_path (DBusMessage   *message,
                       const char    *object_path)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (object_path == NULL ||
                            _dbus_check_is_valid_path (object_path),
                            FALSE);

  return set_or_delete_string_field (message,
                                     DBUS_HEADER_FIELD_PATH,
                                     DBUS_TYPE_OBJECT_PATH,
                                     object_path);
}

/**
 * Gets the object path this message is being sent to (for
 * DBUS_MESSAGE_TYPE_METHOD_CALL) or being emitted from (for
 * DBUS_MESSAGE_TYPE_SIGNAL). Returns #NULL if none.
 *
 * @param message the message
 * @returns the path (should not be freed) or #NULL
 */
const char*
dbus_message_get_path (DBusMessage   *message)
{
  const char *v;

  _dbus_return_val_if_fail (message != NULL, NULL);

  v = NULL; /* in case field doesn't exist */
  _dbus_header_get_field_basic (&message->header,
                                DBUS_HEADER_FIELD_PATH,
                                DBUS_TYPE_OBJECT_PATH,
                                &v);
  return v;
}

/**
 * Checks if the message has a path
 *
 * @param message the message
 * @param path the path name
 * @returns #TRUE if there is a path field in the header
 */
dbus_bool_t
dbus_message_has_path (DBusMessage   *message,
                       const char    *path)
{
  const char *msg_path;
  msg_path = dbus_message_get_path (message);
  
  if (msg_path == NULL)
    {
      if (path == NULL)
        return TRUE;
      else
        return FALSE;
    }

  if (path == NULL)
    return FALSE;
   
  if (strcmp (msg_path, path) == 0)
    return TRUE;

  return FALSE;
}

/**
 * Gets the object path this message is being sent to
 * (for DBUS_MESSAGE_TYPE_METHOD_CALL) or being emitted
 * from (for DBUS_MESSAGE_TYPE_SIGNAL) in a decomposed
 * format (one array element per path component).
 * Free the returned array with dbus_free_string_array().
 *
 * An empty but non-NULL path array means the path "/".
 * So the path "/foo/bar" becomes { "foo", "bar", NULL }
 * and the path "/" becomes { NULL }.
 *
 * @todo this could be optimized by using the len from the message
 * instead of calling strlen() again
 *
 * @param message the message
 * @param path place to store allocated array of path components; #NULL set here if no path field exists
 * @returns #FALSE if no memory to allocate the array
 */
dbus_bool_t
dbus_message_get_path_decomposed (DBusMessage   *message,
                                  char        ***path)
{
  const char *v;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (path != NULL, FALSE);

  *path = NULL;

  v = dbus_message_get_path (message);
  if (v != NULL)
    {
      if (!_dbus_decompose_path (v, strlen (v),
                                 path, NULL))
        return FALSE;
    }
  return TRUE;
}

/**
 * Sets the interface this message is being sent to
 * (for DBUS_MESSAGE_TYPE_METHOD_CALL) or
 * the interface a signal is being emitted from
 * (for DBUS_MESSAGE_TYPE_SIGNAL).
 *
 * @param message the message
 * @param interface the interface or #NULL to unset
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_interface (DBusMessage  *message,
                            const char   *interface)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (interface == NULL ||
                            _dbus_check_is_valid_interface (interface),
                            FALSE);

  return set_or_delete_string_field (message,
                                     DBUS_HEADER_FIELD_INTERFACE,
                                     DBUS_TYPE_STRING,
                                     interface);
}

/**
 * Gets the interface this message is being sent to
 * (for DBUS_MESSAGE_TYPE_METHOD_CALL) or being emitted
 * from (for DBUS_MESSAGE_TYPE_SIGNAL).
 * The interface name is fully-qualified (namespaced).
 * Returns #NULL if none.
 *
 * @param message the message
 * @returns the message interface (should not be freed) or #NULL
 */
const char*
dbus_message_get_interface (DBusMessage *message)
{
  const char *v;

  _dbus_return_val_if_fail (message != NULL, NULL);

  v = NULL; /* in case field doesn't exist */
  _dbus_header_get_field_basic (&message->header,
                                DBUS_HEADER_FIELD_INTERFACE,
                                DBUS_TYPE_STRING,
                                &v);
  return v;
}

/**
 * Checks if the message has an interface
 *
 * @param message the message
 * @param interface the interface name
 * @returns #TRUE if there is a interface field in the header
 */
dbus_bool_t
dbus_message_has_interface (DBusMessage   *message,
                            const char    *interface)
{
  const char *msg_interface;
  msg_interface = dbus_message_get_interface (message);
   
  if (msg_interface == NULL)
    {
      if (interface == NULL)
        return TRUE;
      else
        return FALSE;
    }

  if (interface == NULL)
    return FALSE;
     
  if (strcmp (msg_interface, interface) == 0)
    return TRUE;

  return FALSE;

}

/**
 * Sets the interface member being invoked
 * (DBUS_MESSAGE_TYPE_METHOD_CALL) or emitted
 * (DBUS_MESSAGE_TYPE_SIGNAL).
 * The interface name is fully-qualified (namespaced).
 *
 * @param message the message
 * @param member the member or #NULL to unset
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_member (DBusMessage  *message,
                         const char   *member)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (member == NULL ||
                            _dbus_check_is_valid_member (member),
                            FALSE);

  return set_or_delete_string_field (message,
                                     DBUS_HEADER_FIELD_MEMBER,
                                     DBUS_TYPE_STRING,
                                     member);
}

/**
 * Gets the interface member being invoked
 * (DBUS_MESSAGE_TYPE_METHOD_CALL) or emitted
 * (DBUS_MESSAGE_TYPE_SIGNAL). Returns #NULL if none.
 *
 * @param message the message
 * @returns the member name (should not be freed) or #NULL
 */
const char*
dbus_message_get_member (DBusMessage *message)
{
  const char *v;

  _dbus_return_val_if_fail (message != NULL, NULL);

  v = NULL; /* in case field doesn't exist */
  _dbus_header_get_field_basic (&message->header,
                                DBUS_HEADER_FIELD_MEMBER,
                                DBUS_TYPE_STRING,
                                &v);
  return v;
}

/**
 * Checks if the message has an interface member
 *
 * @param message the message
 * @param member the member name
 * @returns #TRUE if there is a member field in the header
 */
dbus_bool_t
dbus_message_has_member (DBusMessage   *message,
                         const char    *member)
{
  const char *msg_member;
  msg_member = dbus_message_get_member (message);
 
  if (msg_member == NULL)
    {
      if (member == NULL)
        return TRUE;
      else
        return FALSE;
    }

  if (member == NULL)
    return FALSE;
    
  if (strcmp (msg_member, member) == 0)
    return TRUE;

  return FALSE;

}

/**
 * Sets the name of the error (DBUS_MESSAGE_TYPE_ERROR).
 * The name is fully-qualified (namespaced).
 *
 * @param message the message
 * @param error_name the name or #NULL to unset
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_error_name (DBusMessage  *message,
                             const char   *error_name)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (error_name == NULL ||
                            _dbus_check_is_valid_error_name (error_name),
                            FALSE);

  return set_or_delete_string_field (message,
                                     DBUS_HEADER_FIELD_ERROR_NAME,
                                     DBUS_TYPE_STRING,
                                     error_name);
}

/**
 * Gets the error name (DBUS_MESSAGE_TYPE_ERROR only)
 * or #NULL if none.
 *
 * @param message the message
 * @returns the error name (should not be freed) or #NULL
 */
const char*
dbus_message_get_error_name (DBusMessage *message)
{
  const char *v;

  _dbus_return_val_if_fail (message != NULL, NULL);

  v = NULL; /* in case field doesn't exist */
  _dbus_header_get_field_basic (&message->header,
                                DBUS_HEADER_FIELD_ERROR_NAME,
                                DBUS_TYPE_STRING,
                                &v);
  return v;
}

/**
 * Sets the message's destination. The destination is the name of
 * another connection on the bus and may be either the unique name
 * assigned by the bus to each connection, or a well-known name
 * specified in advance.
 *
 * @param message the message
 * @param destination the destination name or #NULL to unset
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_destination (DBusMessage  *message,
                              const char   *destination)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (destination == NULL ||
                            _dbus_check_is_valid_bus_name (destination),
                            FALSE);

  return set_or_delete_string_field (message,
                                     DBUS_HEADER_FIELD_DESTINATION,
                                     DBUS_TYPE_STRING,
                                     destination);
}

/**
 * Gets the destination of a message or #NULL if there is none set.
 *
 * @param message the message
 * @returns the message destination (should not be freed) or #NULL
 */
const char*
dbus_message_get_destination (DBusMessage *message)
{
  const char *v;

  _dbus_return_val_if_fail (message != NULL, NULL);

  v = NULL; /* in case field doesn't exist */
  _dbus_header_get_field_basic (&message->header,
                                DBUS_HEADER_FIELD_DESTINATION,
                                DBUS_TYPE_STRING,
                                &v);
  return v;
}

/**
 * Sets the message sender.
 *
 * @param message the message
 * @param sender the sender or #NULL to unset
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_sender (DBusMessage  *message,
                         const char   *sender)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (!message->locked, FALSE);
  _dbus_return_val_if_fail (sender == NULL ||
                            _dbus_check_is_valid_bus_name (sender),
                            FALSE);

  return set_or_delete_string_field (message,
                                     DBUS_HEADER_FIELD_SENDER,
                                     DBUS_TYPE_STRING,
                                     sender);
}

/**
 * Gets the unique name of the connection which originated this
 * message, or #NULL if unknown or inapplicable. The sender is filled
 * in by the message bus.
 *
 * @param message the message
 * @returns the unique name of the sender or #NULL
 */
const char*
dbus_message_get_sender (DBusMessage *message)
{
  const char *v;

  _dbus_return_val_if_fail (message != NULL, NULL);

  v = NULL; /* in case field doesn't exist */
  _dbus_header_get_field_basic (&message->header,
                                DBUS_HEADER_FIELD_SENDER,
                                DBUS_TYPE_STRING,
                                &v);
  return v;
}

/**
 * Gets the type signature of the message, i.e. the arguments in the
 * message payload. The signature includes only "in" arguments for
 * #DBUS_MESSAGE_TYPE_METHOD_CALL and only "out" arguments for
 * #DBUS_MESSAGE_TYPE_METHOD_RETURN, so is slightly different from
 * what you might expect (it does not include the signature of the
 * entire C++-style method).
 *
 * The signature is a string made up of type codes such as
 * #DBUS_TYPE_INT32. The string is terminated with nul (nul is also
 * the value of #DBUS_TYPE_INVALID).
 *
 * @param message the message
 * @returns the type signature
 */
const char*
dbus_message_get_signature (DBusMessage *message)
{
  const DBusString *type_str;
  int type_pos;

  _dbus_return_val_if_fail (message != NULL, NULL);

  get_const_signature (&message->header, &type_str, &type_pos);

  return _dbus_string_get_const_data_len (type_str, type_pos, 0);
}

static dbus_bool_t
_dbus_message_has_type_interface_member (DBusMessage *message,
                                         int          type,
                                         const char  *interface,
                                         const char  *member)
{
  const char *n;

  _dbus_assert (message != NULL);
  _dbus_assert (interface != NULL);
  _dbus_assert (member != NULL);

  if (dbus_message_get_type (message) != type)
    return FALSE;

  /* Optimize by checking the short member name first
   * instead of the longer interface name
   */

  n = dbus_message_get_member (message);

  if (n && strcmp (n, member) == 0)
    {
      n = dbus_message_get_interface (message);

      if (n == NULL || strcmp (n, interface) == 0)
        return TRUE;
    }

  return FALSE;
}

/**
 * Checks whether the message is a method call with the given
 * interface and member fields.  If the message is not
 * #DBUS_MESSAGE_TYPE_METHOD_CALL, or has a different interface or
 * member field, returns #FALSE. If the interface field is missing,
 * then it will be assumed equal to the provided interface.  The D-Bus
 * protocol allows method callers to leave out the interface name.
 *
 * @param message the message
 * @param interface the name to check (must not be #NULL)
 * @param method the name to check (must not be #NULL)
 *
 * @returns #TRUE if the message is the specified method call
 */
dbus_bool_t
dbus_message_is_method_call (DBusMessage *message,
                             const char  *interface,
                             const char  *method)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (interface != NULL, FALSE);
  _dbus_return_val_if_fail (method != NULL, FALSE);
  /* don't check that interface/method are valid since it would be
   * expensive, and not catch many common errors
   */

  return _dbus_message_has_type_interface_member (message,
                                                  DBUS_MESSAGE_TYPE_METHOD_CALL,
                                                  interface, method);
}

/**
 * Checks whether the message is a signal with the given interface and
 * member fields.  If the message is not #DBUS_MESSAGE_TYPE_SIGNAL, or
 * has a different interface or member field, returns #FALSE.  If the
 * interface field in the message is missing, it is assumed to match
 * any interface you pass in to this function.
 *
 * @param message the message
 * @param interface the name to check (must not be #NULL)
 * @param signal_name the name to check (must not be #NULL)
 *
 * @returns #TRUE if the message is the specified signal
 */
dbus_bool_t
dbus_message_is_signal (DBusMessage *message,
                        const char  *interface,
                        const char  *signal_name)
{
  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (interface != NULL, FALSE);
  _dbus_return_val_if_fail (signal_name != NULL, FALSE);
  /* don't check that interface/name are valid since it would be
   * expensive, and not catch many common errors
   */

  return _dbus_message_has_type_interface_member (message,
                                                  DBUS_MESSAGE_TYPE_SIGNAL,
                                                  interface, signal_name);
}

/**
 * Checks whether the message is an error reply with the given error
 * name.  If the message is not #DBUS_MESSAGE_TYPE_ERROR, or has a
 * different name, returns #FALSE.
 *
 * @param message the message
 * @param error_name the name to check (must not be #NULL)
 *
 * @returns #TRUE if the message is the specified error
 */
dbus_bool_t
dbus_message_is_error (DBusMessage *message,
                       const char  *error_name)
{
  const char *n;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (error_name != NULL, FALSE);
  /* don't check that error_name is valid since it would be expensive,
   * and not catch many common errors
   */

  if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_ERROR)
    return FALSE;

  n = dbus_message_get_error_name (message);

  if (n && strcmp (n, error_name) == 0)
    return TRUE;
  else
    return FALSE;
}

/**
 * Checks whether the message was sent to the given name.  If the
 * message has no destination specified or has a different
 * destination, returns #FALSE.
 *
 * @param message the message
 * @param name the name to check (must not be #NULL)
 *
 * @returns #TRUE if the message has the given destination name
 */
dbus_bool_t
dbus_message_has_destination (DBusMessage  *message,
                              const char   *name)
{
  const char *s;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (name != NULL, FALSE);
  /* don't check that name is valid since it would be expensive, and
   * not catch many common errors
   */

  s = dbus_message_get_destination (message);

  if (s && strcmp (s, name) == 0)
    return TRUE;
  else
    return FALSE;
}

/**
 * Checks whether the message has the given unique name as its sender.
 * If the message has no sender specified or has a different sender,
 * returns #FALSE. Note that a peer application will always have the
 * unique name of the connection as the sender. So you can't use this
 * function to see whether a sender owned a well-known name.
 *
 * Messages from the bus itself will have #DBUS_SERVICE_DBUS
 * as the sender.
 *
 * @param message the message
 * @param name the name to check (must not be #NULL)
 *
 * @returns #TRUE if the message has the given sender
 */
dbus_bool_t
dbus_message_has_sender (DBusMessage  *message,
                         const char   *name)
{
  const char *s;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (name != NULL, FALSE);
  /* don't check that name is valid since it would be expensive, and
   * not catch many common errors
   */

  s = dbus_message_get_sender (message);

  if (s && strcmp (s, name) == 0)
    return TRUE;
  else
    return FALSE;
}

/**
 * Checks whether the message has the given signature; see
 * dbus_message_get_signature() for more details on what the signature
 * looks like.
 *
 * @param message the message
 * @param signature typecode array
 * @returns #TRUE if message has the given signature
*/
dbus_bool_t
dbus_message_has_signature (DBusMessage   *message,
                            const char    *signature)
{
  const char *s;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (signature != NULL, FALSE);
  /* don't check that signature is valid since it would be expensive,
   * and not catch many common errors
   */

  s = dbus_message_get_signature (message);

  if (s && strcmp (s, signature) == 0)
    return TRUE;
  else
    return FALSE;
}

/**
 * Sets a #DBusError based on the contents of the given
 * message. The error is only set if the message
 * is an error message, as in DBUS_MESSAGE_TYPE_ERROR.
 * The name of the error is set to the name of the message,
 * and the error message is set to the first argument
 * if the argument exists and is a string.
 *
 * The return value indicates whether the error was set (the error is
 * set if and only if the message is an error message).  So you can
 * check for an error reply and convert it to DBusError in one go:
 * @code
 *  if (dbus_set_error_from_message (error, reply))
 *    return error;
 *  else
 *    process reply;
 * @endcode
 *
 * @param error the error to set
 * @param message the message to set it from
 * @returns #TRUE if dbus_message_get_is_error() returns #TRUE for the message
 */
dbus_bool_t
dbus_set_error_from_message (DBusError   *error,
                             DBusMessage *message)
{
  const char *str;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_error_is_set (error, FALSE);

  if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_ERROR)
    return FALSE;

  str = NULL;
  dbus_message_get_args (message, NULL,
                         DBUS_TYPE_STRING, &str,
                         DBUS_TYPE_INVALID);

  dbus_set_error (error, dbus_message_get_error_name (message),
                  str ? "%s" : NULL, str);

  return TRUE;
}

/** @} */

/**
 * @addtogroup DBusMessageInternals
 *
 * @{
 */

/**
 * The initial buffer size of the message loader.
 *
 * @todo this should be based on min header size plus some average
 * body size, or something. Or rather, the min header size only, if we
 * want to try to read only the header, store that in a DBusMessage,
 * then read only the body and store that, etc., depends on
 * how we optimize _dbus_message_loader_get_buffer() and what
 * the exact message format is.
 */
#define INITIAL_LOADER_DATA_LEN 32

/**
 * Creates a new message loader. Returns #NULL if memory can't
 * be allocated.
 *
 * @returns new loader, or #NULL.
 */
DBusMessageLoader*
_dbus_message_loader_new (void)
{
  DBusMessageLoader *loader;

  loader = dbus_new0 (DBusMessageLoader, 1);
  if (loader == NULL)
    return NULL;
  
  loader->refcount = 1;

  loader->corrupted = FALSE;
  loader->corruption_reason = DBUS_VALID;

  /* this can be configured by the app, but defaults to the protocol max */
  loader->max_message_size = DBUS_MAXIMUM_MESSAGE_LENGTH;

  if (!_dbus_string_init (&loader->data))
    {
      dbus_free (loader);
      return NULL;
    }

  /* preallocate the buffer for speed, ignore failure */
  _dbus_string_set_length (&loader->data, INITIAL_LOADER_DATA_LEN);
  _dbus_string_set_length (&loader->data, 0);

  return loader;
}

/**
 * Increments the reference count of the loader.
 *
 * @param loader the loader.
 * @returns the loader
 */
DBusMessageLoader *
_dbus_message_loader_ref (DBusMessageLoader *loader)
{
  loader->refcount += 1;

  return loader;
}

/**
 * Decrements the reference count of the loader and finalizes the
 * loader when the count reaches zero.
 *
 * @param loader the loader.
 */
void
_dbus_message_loader_unref (DBusMessageLoader *loader)
{
  loader->refcount -= 1;
  if (loader->refcount == 0)
    {
      _dbus_list_foreach (&loader->messages,
                          (DBusForeachFunction) dbus_message_unref,
                          NULL);
      _dbus_list_clear (&loader->messages);
      _dbus_string_free (&loader->data);
      dbus_free (loader);
    }
}

/**
 * Gets the buffer to use for reading data from the network.  Network
 * data is read directly into an allocated buffer, which is then used
 * in the DBusMessage, to avoid as many extra memcpy's as possible.
 * The buffer must always be returned immediately using
 * _dbus_message_loader_return_buffer(), even if no bytes are
 * successfully read.
 *
 * @todo this function can be a lot more clever. For example
 * it can probably always return a buffer size to read exactly
 * the body of the next message, thus avoiding any memory wastage
 * or reallocs.
 *
 * @todo we need to enforce a max length on strings in header fields.
 *
 * @param loader the message loader.
 * @param buffer the buffer
 */
void
_dbus_message_loader_get_buffer (DBusMessageLoader  *loader,
                                 DBusString        **buffer)
{
  _dbus_assert (!loader->buffer_outstanding);

  *buffer = &loader->data;

  loader->buffer_outstanding = TRUE;
}

/**
 * Returns a buffer obtained from _dbus_message_loader_get_buffer(),
 * indicating to the loader how many bytes of the buffer were filled
 * in. This function must always be called, even if no bytes were
 * successfully read.
 *
 * @param loader the loader.
 * @param buffer the buffer.
 * @param bytes_read number of bytes that were read into the buffer.
 */
void
_dbus_message_loader_return_buffer (DBusMessageLoader  *loader,
                                    DBusString         *buffer,
                                    int                 bytes_read)
{
  _dbus_assert (loader->buffer_outstanding);
  _dbus_assert (buffer == &loader->data);

  loader->buffer_outstanding = FALSE;
}

/*
 * FIXME when we move the header out of the buffer, that memmoves all
 * buffered messages. Kind of crappy.
 *
 * Also we copy the header and body, which is kind of crappy.  To
 * avoid this, we have to allow header and body to be in a single
 * memory block, which is good for messages we read and bad for
 * messages we are creating. But we could move_len() the buffer into
 * this single memory block, and move_len() will just swap the buffers
 * if you're moving the entire buffer replacing the dest string.
 *
 * We could also have the message loader tell the transport how many
 * bytes to read; so it would first ask for some arbitrary number like
 * 256, then if the message was incomplete it would use the
 * header/body len to ask for exactly the size of the message (or
 * blocks the size of a typical kernel buffer for the socket). That
 * way we don't get trailing bytes in the buffer that have to be
 * memmoved. Though I suppose we also don't have a chance of reading a
 * bunch of small messages at once, so the optimization may be stupid.
 *
 * Another approach would be to keep a "start" index into
 * loader->data and only delete it occasionally, instead of after
 * each message is loaded.
 *
 * load_message() returns FALSE if not enough memory OR the loader was corrupted
 */
static dbus_bool_t
load_message (DBusMessageLoader *loader,
              DBusMessage       *message,
              int                byte_order,
              int                fields_array_len,
              int                header_len,
              int                body_len)
{
  dbus_bool_t oom;
  DBusValidity validity;
  const DBusString *type_str;
  int type_pos;
  DBusValidationMode mode;

  mode = DBUS_VALIDATION_MODE_DATA_IS_UNTRUSTED;
  
  oom = FALSE;

#if 0
  _dbus_verbose_bytes_of_string (&loader->data, 0, header_len /* + body_len */);
#endif

  /* 1. VALIDATE AND COPY OVER HEADER */
  _dbus_assert (_dbus_string_get_length (&message->header.data) == 0);
  _dbus_assert ((header_len + body_len) <= _dbus_string_get_length (&loader->data));

  if (!_dbus_header_load (&message->header,
                          mode,
                          &validity,
                          byte_order,
                          fields_array_len,
                          header_len,
                          body_len,
                          &loader->data, 0,
                          _dbus_string_get_length (&loader->data)))
    {
      _dbus_verbose ("Failed to load header for new message code %d\n", validity);

      /* assert here so we can catch any code that still uses DBUS_VALID to indicate
         oom errors.  They should use DBUS_VALIDITY_UNKNOWN_OOM_ERROR instead */
      _dbus_assert (validity != DBUS_VALID);

      if (validity == DBUS_VALIDITY_UNKNOWN_OOM_ERROR)
        oom = TRUE;
      else
        {
          loader->corrupted = TRUE;
          loader->corruption_reason = validity;
        }
      goto failed;
    }

  _dbus_assert (validity == DBUS_VALID);

  message->byte_order = byte_order;

  /* 2. VALIDATE BODY */
  if (mode != DBUS_VALIDATION_MODE_WE_TRUST_THIS_DATA_ABSOLUTELY)
    {
      get_const_signature (&message->header, &type_str, &type_pos);
      
      /* Because the bytes_remaining arg is NULL, this validates that the
       * body is the right length
       */
      validity = _dbus_validate_body_with_reason (type_str,
                                                  type_pos,
                                                  byte_order,
                                                  NULL,
                                                  &loader->data,
                                                  header_len,
                                                  body_len);
      if (validity != DBUS_VALID)
        {
          _dbus_verbose ("Failed to validate message body code %d\n", validity);

          loader->corrupted = TRUE;
          loader->corruption_reason = validity;
          
          goto failed;
        }
    }

  /* 3. COPY OVER BODY AND QUEUE MESSAGE */

  if (!_dbus_list_append (&loader->messages, message))
    {
      _dbus_verbose ("Failed to append new message to loader queue\n");
      oom = TRUE;
      goto failed;
    }

  _dbus_assert (_dbus_string_get_length (&message->body) == 0);
  _dbus_assert (_dbus_string_get_length (&loader->data) >=
                (header_len + body_len));

  if (!_dbus_string_copy_len (&loader->data, header_len, body_len, &message->body, 0))
    {
      _dbus_verbose ("Failed to move body into new message\n");
      oom = TRUE;
      goto failed;
    }

  _dbus_string_delete (&loader->data, 0, header_len + body_len);

  _dbus_assert (_dbus_string_get_length (&message->header.data) == header_len);
  _dbus_assert (_dbus_string_get_length (&message->body) == body_len);

  _dbus_verbose ("Loaded message %p\n", message);

  _dbus_assert (!oom);
  _dbus_assert (!loader->corrupted);
  _dbus_assert (loader->messages != NULL);
  _dbus_assert (_dbus_list_find_last (&loader->messages, message) != NULL);

  return TRUE;

 failed:

  /* Clean up */

  /* does nothing if the message isn't in the list */
  _dbus_list_remove_last (&loader->messages, message);
  
  if (oom)
    _dbus_assert (!loader->corrupted);
  else
    _dbus_assert (loader->corrupted);

  _dbus_verbose_bytes_of_string (&loader->data, 0, _dbus_string_get_length (&loader->data));

  return FALSE;
}

/**
 * Converts buffered data into messages, if we have enough data.  If
 * we don't have enough data, does nothing.
 *
 * @todo we need to check that the proper named header fields exist
 * for each message type.
 *
 * @todo If a message has unknown type, we should probably eat it
 * right here rather than passing it out to applications.  However
 * it's not an error to see messages of unknown type.
 *
 * @param loader the loader.
 * @returns #TRUE if we had enough memory to finish.
 */
dbus_bool_t
_dbus_message_loader_queue_messages (DBusMessageLoader *loader)
{
  while (!loader->corrupted &&
         _dbus_string_get_length (&loader->data) >= DBUS_MINIMUM_HEADER_SIZE)
    {
      DBusValidity validity;
      int byte_order, fields_array_len, header_len, body_len;

      if (_dbus_header_have_message_untrusted (loader->max_message_size,
                                               &validity,
                                               &byte_order,
                                               &fields_array_len,
                                               &header_len,
                                               &body_len,
                                               &loader->data, 0,
                                               _dbus_string_get_length (&loader->data)))
        {
          DBusMessage *message;

          _dbus_assert (validity == DBUS_VALID);

          message = dbus_message_new_empty_header ();
          if (message == NULL)
            return FALSE;

          if (!load_message (loader, message,
                             byte_order, fields_array_len,
                             header_len, body_len))
            {
              dbus_message_unref (message);
              /* load_message() returns false if corrupted or OOM; if
               * corrupted then return TRUE for not OOM
               */
              return loader->corrupted;
            }

          _dbus_assert (loader->messages != NULL);
          _dbus_assert (_dbus_list_find_last (&loader->messages, message) != NULL);
	}
      else
        {
          _dbus_verbose ("Initial peek at header says we don't have a whole message yet, or data broken with invalid code %d\n",
                         validity);
          if (validity != DBUS_VALID)
            {
              loader->corrupted = TRUE;
              loader->corruption_reason = validity;
            }
          return TRUE;
        }
    }

  return TRUE;
}

/**
 * Peeks at first loaded message, returns #NULL if no messages have
 * been queued.
 *
 * @param loader the loader.
 * @returns the next message, or #NULL if none.
 */
DBusMessage*
_dbus_message_loader_peek_message (DBusMessageLoader *loader)
{
  if (loader->messages)
    return loader->messages->data;
  else
    return NULL;
}

/**
 * Pops a loaded message (passing ownership of the message
 * to the caller). Returns #NULL if no messages have been
 * queued.
 *
 * @param loader the loader.
 * @returns the next message, or #NULL if none.
 */
DBusMessage*
_dbus_message_loader_pop_message (DBusMessageLoader *loader)
{
  return _dbus_list_pop_first (&loader->messages);
}

/**
 * Pops a loaded message inside a list link (passing ownership of the
 * message and link to the caller). Returns #NULL if no messages have
 * been loaded.
 *
 * @param loader the loader.
 * @returns the next message link, or #NULL if none.
 */
DBusList*
_dbus_message_loader_pop_message_link (DBusMessageLoader *loader)
{
  return _dbus_list_pop_first_link (&loader->messages);
}

/**
 * Returns a popped message link, used to undo a pop.
 *
 * @param loader the loader
 * @param link the link with a message in it
 */
void
_dbus_message_loader_putback_message_link (DBusMessageLoader  *loader,
                                           DBusList           *link)
{
  _dbus_list_prepend_link (&loader->messages, link);
}

/**
 * Checks whether the loader is confused due to bad data.
 * If messages are received that are invalid, the
 * loader gets confused and gives up permanently.
 * This state is called "corrupted."
 *
 * @param loader the loader
 * @returns #TRUE if the loader is hosed.
 */
dbus_bool_t
_dbus_message_loader_get_is_corrupted (DBusMessageLoader *loader)
{
  _dbus_assert ((loader->corrupted && loader->corruption_reason != DBUS_VALID) ||
                (!loader->corrupted && loader->corruption_reason == DBUS_VALID));
  return loader->corrupted;
}

/**
 * Sets the maximum size message we allow.
 *
 * @param loader the loader
 * @param size the max message size in bytes
 */
void
_dbus_message_loader_set_max_message_size (DBusMessageLoader  *loader,
                                           long                size)
{
  if (size > DBUS_MAXIMUM_MESSAGE_LENGTH)
    {
      _dbus_verbose ("clamping requested max message size %ld to %d\n",
                     size, DBUS_MAXIMUM_MESSAGE_LENGTH);
      size = DBUS_MAXIMUM_MESSAGE_LENGTH;
    }
  loader->max_message_size = size;
}

/**
 * Gets the maximum allowed message size in bytes.
 *
 * @param loader the loader
 * @returns max size in bytes
 */
long
_dbus_message_loader_get_max_message_size (DBusMessageLoader  *loader)
{
  return loader->max_message_size;
}

static DBusDataSlotAllocator slot_allocator;
_DBUS_DEFINE_GLOBAL_LOCK (message_slots);

/**
 * Allocates an integer ID to be used for storing application-specific
 * data on any DBusMessage. The allocated ID may then be used
 * with dbus_message_set_data() and dbus_message_get_data().
 * The passed-in slot must be initialized to -1, and is filled in
 * with the slot ID. If the passed-in slot is not -1, it's assumed
 * to be already allocated, and its refcount is incremented.
 *
 * The allocated slot is global, i.e. all DBusMessage objects will
 * have a slot with the given integer ID reserved.
 *
 * @param slot_p address of a global variable storing the slot
 * @returns #FALSE on failure (no memory)
 */
dbus_bool_t
dbus_message_allocate_data_slot (dbus_int32_t *slot_p)
{
  return _dbus_data_slot_allocator_alloc (&slot_allocator,
                                          &_DBUS_LOCK_NAME (message_slots),
                                          slot_p);
}

/**
 * Deallocates a global ID for message data slots.
 * dbus_message_get_data() and dbus_message_set_data() may no
 * longer be used with this slot.  Existing data stored on existing
 * DBusMessage objects will be freed when the message is
 * finalized, but may not be retrieved (and may only be replaced if
 * someone else reallocates the slot).  When the refcount on the
 * passed-in slot reaches 0, it is set to -1.
 *
 * @param slot_p address storing the slot to deallocate
 */
void
dbus_message_free_data_slot (dbus_int32_t *slot_p)
{
  _dbus_return_if_fail (*slot_p >= 0);

  _dbus_data_slot_allocator_free (&slot_allocator, slot_p);
}

/**
 * Stores a pointer on a DBusMessage, along
 * with an optional function to be used for freeing
 * the data when the data is set again, or when
 * the message is finalized. The slot number
 * must have been allocated with dbus_message_allocate_data_slot().
 *
 * @param message the message
 * @param slot the slot number
 * @param data the data to store
 * @param free_data_func finalizer function for the data
 * @returns #TRUE if there was enough memory to store the data
 */
dbus_bool_t
dbus_message_set_data (DBusMessage     *message,
                       dbus_int32_t     slot,
                       void            *data,
                       DBusFreeFunction free_data_func)
{
  DBusFreeFunction old_free_func;
  void *old_data;
  dbus_bool_t retval;

  _dbus_return_val_if_fail (message != NULL, FALSE);
  _dbus_return_val_if_fail (slot >= 0, FALSE);

  retval = _dbus_data_slot_list_set (&slot_allocator,
                                     &message->slot_list,
                                     slot, data, free_data_func,
                                     &old_free_func, &old_data);

  if (retval)
    {
      /* Do the actual free outside the message lock */
      if (old_free_func)
        (* old_free_func) (old_data);
    }

  return retval;
}

/**
 * Retrieves data previously set with dbus_message_set_data().
 * The slot must still be allocated (must not have been freed).
 *
 * @param message the message
 * @param slot the slot to get data from
 * @returns the data, or #NULL if not found
 */
void*
dbus_message_get_data (DBusMessage   *message,
                       dbus_int32_t   slot)
{
  void *res;

  _dbus_return_val_if_fail (message != NULL, NULL);

  res = _dbus_data_slot_list_get (&slot_allocator,
                                  &message->slot_list,
                                  slot);

  return res;
}

/**
 * Utility function to convert a machine-readable (not translated)
 * string into a D-Bus message type.
 *
 * @code
 *   "method_call"    -> DBUS_MESSAGE_TYPE_METHOD_CALL
 *   "method_return"  -> DBUS_MESSAGE_TYPE_METHOD_RETURN
 *   "signal"         -> DBUS_MESSAGE_TYPE_SIGNAL
 *   "error"          -> DBUS_MESSAGE_TYPE_ERROR
 *   anything else    -> DBUS_MESSAGE_TYPE_INVALID
 * @endcode
 *
 */
int
dbus_message_type_from_string (const char *type_str)
{
  if (strcmp (type_str, "method_call") == 0)
    return DBUS_MESSAGE_TYPE_METHOD_CALL;
  if (strcmp (type_str, "method_return") == 0)
    return DBUS_MESSAGE_TYPE_METHOD_RETURN;
  else if (strcmp (type_str, "signal") == 0)
    return DBUS_MESSAGE_TYPE_SIGNAL;
  else if (strcmp (type_str, "error") == 0)
    return DBUS_MESSAGE_TYPE_ERROR;
  else
    return DBUS_MESSAGE_TYPE_INVALID;
}

/**
 * Utility function to convert a D-Bus message type into a
 * machine-readable string (not translated).
 *
 * @code
 *   DBUS_MESSAGE_TYPE_METHOD_CALL    -> "method_call"
 *   DBUS_MESSAGE_TYPE_METHOD_RETURN  -> "method_return"
 *   DBUS_MESSAGE_TYPE_SIGNAL         -> "signal"
 *   DBUS_MESSAGE_TYPE_ERROR          -> "error"
 *   DBUS_MESSAGE_TYPE_INVALID        -> "invalid"
 * @endcode
 *
 */
const char *
dbus_message_type_to_string (int type)
{
  switch (type)
    {
    case DBUS_MESSAGE_TYPE_METHOD_CALL:
      return "method_call";
    case DBUS_MESSAGE_TYPE_METHOD_RETURN:
      return "method_return";
    case DBUS_MESSAGE_TYPE_SIGNAL:
      return "signal";
    case DBUS_MESSAGE_TYPE_ERROR:
      return "error";
    default:
      return "invalid";
    }
}

/** @} */

/* tests in dbus-message-util.c */
