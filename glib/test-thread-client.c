#include <glib.h>
#include "dbus-glib.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test-thread.h"

DBusConnection *connection;

static  gpointer
thread_func (gpointer data)
{
  gint32 threadnr = GPOINTER_TO_INT (data);
  guint32 counter = 0;
  DBusMessageIter iter;
  DBusMessage *message;
  char *str;

  while (1)
    {
      message = dbus_message_new_method_call ("org.freedesktop.ThreadTest",
                                              "TestMethod", NULL);

      dbus_message_append_iter_init (message, &iter);

      if (!dbus_message_iter_append_int32 (&iter, threadnr))
	{
	  g_print ("thread %d: append threadnr failed\n", threadnr);
	}
      
      if (!dbus_message_iter_append_uint32 (&iter, counter))
	{
	  g_print ("thread %d: append counter (%d) failed\n", threadnr, counter);
	}
      
      str = g_strdup_printf ("Thread %d-%d\n", threadnr, counter);
      if (!dbus_message_iter_append_string (&iter, str))
	{
	  g_print ("thread %d: append string (%s) failed\n", threadnr, str);
	}
      g_free (str);

      if (!dbus_connection_send (connection,
                                 message,
                                 NULL))
	{
	  g_print ("thread %d: send message failed\n", threadnr);
	}
      
      dbus_message_unref (message);
      
      counter ++;
    }

  return NULL;
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  DBusError error;
  int i;

  g_thread_init (NULL);
  dbus_gthread_init ();

  if(argc < 2)
    {
      g_error("Need an address as argv[1]\n");
      return 1;
    }

  dbus_error_init (&error);
  connection = dbus_connection_open (argv[1], &error);
  if (connection == NULL)
    {
      g_printerr ("could not open connection: %s\n", error.message);
      dbus_error_free (&error);
      return 1;
    }

  dbus_connection_setup_with_g_main (connection, NULL);

  for (i = 0; i < N_TEST_THREADS; i++)
    {
      g_thread_create (thread_func, GINT_TO_POINTER (i), FALSE, NULL);
    }

  loop = g_main_loop_new (NULL, FALSE);
  g_main_run (loop);  
  
  return 0;
}
  
