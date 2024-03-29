#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fnmatch.h>
#include <signal.h>

#include <libut/ut.h>
#include <sqlite3.h>
#include <libconfig.h>

#include "bstrlib.h"

// Types

/* Struct describing a limit key.
 * 
 * A key belongs to a class (see below), and contains
 * a count/time pair (ex. 10 times in 90 seconds)
 * and a name that's matched using fnmatch
 * against the client-provided data
 */

typedef struct rkey_t
{
  const char *name;
  bstring report;
  long time;
  long count;
  struct rkey_t *next;
} rkey_t;

/* Struct describing a class.
 * 
 * A class is simply a container of keys,
 * so you can have the same key for different
 * purposes. (ex. joe as a username or joe as a hostname
 * is joe in two different classes.)
 *
 * Each class has a name and a linked list of keys.
 */

typedef struct class_t
{
  char *name;
  struct class_t *next;
  struct rkey_t *keys;
} class_t;


// Global variables

sqlite3 *db;
config_t conf;
class_t *class_list = NULL;
class_t *class_tmp = NULL;
unsigned long max_age = 30;
const char *db_path = 0;
const char *address = 0;
long int port = 0;
const char *control_address = 0;
long int control_port = 0;
long int expiration_timer = 0;
const char *log=0;
long int log_level=0;


// Global constants

struct tagbstring sq = bsStatic ("'");
struct tagbstring dq = bsStatic ("''");
const char *loopback = "127.0.0.1";
const char *memory_db = ":memory:";
const char *dev_stderr = "/dev/stderr";

/* clean_old_marks
 *
 * Called by a timer, it removes all marks older than
 * (global) max_age seconds.
 *
 */

int
clean_old_marks (char *name, unsigned msec, void *data)
{
  UT_LOG (Debug, "Starting cleanup");
  char *zErrMsg = 0;
  bstring query = bformat ("DELETE FROM 'items' where timestamp < %ld;",
			   time (NULL) - max_age);

  UT_LOG (Debug, "SQL: %s", query->data);
  int rc = sqlite3_exec (db, query->data, 0, 0, &zErrMsg);

  if (rc != SQLITE_OK)
  {
    UT_LOG (Error, "SQL error: %s\n", zErrMsg);
    sqlite3_free (zErrMsg);
  }
  bdestroy (query);
  UT_LOG (Debug, "Ending cleanup");
  return 0;
}

/* signal_handler
 *
 * When we get any signal, close the DB, 
 * log what happened and die.
 *
 * TODO: Don't die on all signals
 * TODO: Other cleanups?
 */
int
signal_handler (int signum)
{
  sqlite3_close (db);
  config_destroy (&conf);
  UT_LOG (Fatal, "Got Signal %d", signum);
  return 0;
}

/* check_rate
 * 
 * A callback used when we query for the count against time for a 
 * given key. Stores the count in the arbitrary void * count.
 */

int
check_rate (void *count, int columns, char **result, char **colnames)
{
  *((long *) count) = atol (result[0]);
  return 0;
}

/* Store a mark in the DB for this value and class,
 * timestamped now.
 * 
 * Takes as argument a value and a class.
 * For example, class could be "ip" and value "10.0.0.4"
 * these marks are what's counted later to decide if
 * the rate for this value and class is exceeded
 * or not.
 */

void
mark (const char *value, const char *class)
{

  char *zErrMsg = 0;
  bstring query =
	  bformat ("INSERT INTO 'items' ('value','class','timestamp')"
		   "VALUES ('%s','%s','%ld');",
		   value, class, time (NULL));

  UT_LOG (Debug, "SQL: %s", query->data);
  int rc = sqlite3_exec (db, query->data, 0, 0, &zErrMsg);

  if (rc != SQLITE_OK)
  {
    UT_LOG (Error, "SQL error: %s\n", zErrMsg);
    sqlite3_free (zErrMsg);
  }
  bdestroy (query);
}

/* rate
 *
 * Takes as argument a buffer containing a line of the form
 *
 * class value 
 *
 * and must decide if that combination is over rate or not.
 * 
 * Returns the response message in the msg parameter in these forms:
 *
 * If rate is not exceeded, and this is the first of ten allowed marks:
 *
 * 0 1/10 
 *
 * If rate is exceeded and this is the 12th of 10 allowed marks:
 *
 * 1 12/10
 *
 * If there was an error:
 *
 * 2 Error message here.
 *
 */

int
rate (char *buffer, bstring * msg)
{
  // Find the first space
  char *sp = index (buffer, ' ');

  if (!sp)
  {
    UT_LOG (Info, "2 Bad Input (no space)");
    bassignformat (*msg, "2 Bad Input (no space)");
    return 1;
  }

  bstring value = bfromcstr (sp + 1);

  *sp = 0;
  bstring cl = bfromcstr (buffer);

  bfindreplace (value, &sq, &dq, 0);
  bfindreplace (cl, &sq, &dq, 0);

  UT_LOG (Debug, "Input: %s , %s", cl->data, value->data);
  class_tmp = NULL;
  LL_FIND (class_list, class_tmp, cl->data);
  if (class_tmp)		// Found it
  {
    UT_LOG (Debug, "Class found: %s",cl->data);

    // Iterate over keys trying to match the given string

    rkey_t *tmp, *key = class_tmp->keys;

    while (key)
    {
      if (0 == fnmatch (key->name, value->data, 0))
      {
	UT_LOG (Debug, "Match: %s -- %s %ld %ld", value->data,
		key->name, key->time, key->count);
	// Add mark for current check
	mark (value->data, cl->data);
	// And now see if we are over limited rate

	time_t check_from = time (NULL) - key->time;
	bstring query =
		bformat
		("select COUNT (*) from items "
		 "where value='%s' and timestamp > %ld;",
		 value->data, check_from);
	char *zErrMsg = 0;

	UT_LOG (Debug, "SQL: %s", query->data);
	long count;
	int rc = sqlite3_exec (db, query->data, check_rate, &count, &zErrMsg);

	if (rc != SQLITE_OK)
	{
	  UT_LOG (Error, "SQL error: %s\n", zErrMsg);
	  sqlite3_free (zErrMsg);
	}
	bdestroy (query);


	if (count > key->count)
	{
	  // If the count is exceeded, give an error with what you want reported 
	  bassignformat (*msg, "1 %ld/%ld", count, key->count);
	  UT_LOG (Info, "Rate exceeded: %s", (*msg)->data);
	}
	else
	{
	  // Rate not exceeded, return with informative message
	  bassignformat (*msg, "0 %ld/%ld", count, key->count);
	  UT_LOG (Info, "Rate OK: %s", (*msg)->data);
	}
	break;
      }
      key = key->next;
    }

  }
  else
  {
    UT_LOG (Error, "Class not found %s", buffer);
    bassignformat (*msg, "2 Class not found: %s", buffer);
  }
  bdestroy (cl);
  bdestroy (value);
  return 1;
}


/* handle
 *
 * The network event handler.
 * 
 * Keeps a per-descriptor buffer allocated.
 * When the buffer contains a whole line, it calls rate (buffer,msg)
 * Then sends msg over the descriptor and closes it.
 * 
 */

int
handle (int fd, char *name, int flags, void *b)
{
  bstring buffer = (bstring) b;
  int rc;
  char buf[100];

  if (flags & UTFD_IS_NEWACCEPT)
  {
    buffer = bfromcstr ("");
    UT_fd_cntl (fd, UTFD_SET_DATA, buffer);
    return 0;
  }

  /* socket is readable */
  while ((rc = read (fd, buf, 100)) > 0)
  {
    int count = buffer->slen + rc;

    if (count > 1000)
    {
      // Line is too long
      UT_LOG (Error, "Line too long (%d bytes)", count);
      UT_fd_write (fd, "1 Line is too long\r\n", 20);
      UT_fd_unreg (fd);
      close (fd);
      bdestroy (buffer);
      return 0;
    }
    // Search for \n
    char *el = index (buf, '\n');

    if (el)
    {
      rc = el - buf;
      bcatblk (buffer, buf, rc);
      UT_LOG (Debug, "Checking %s", buffer);
      bstring msg = bfromcstr ("");

      rate (buffer->data, &msg);
      bcatcstr (msg, "\r\n");
      UT_fd_write (fd, msg->data, msg->slen);
      bdestroy (msg);
      UT_fd_unreg (fd);
      close (fd);
      bdestroy (buffer);
      return 0;
    }
    else
    {
      bcatcstr (buffer, buf);
    }
  }
  if (rc == 0 || (rc == -1 && errno != EINTR && errno != EAGAIN))
  {
    UT_LOG (Info, "%s", strerror (errno));
    UT_fd_unreg (fd);
    close (fd);
    bdestroy (buffer);
  }
  return 0;

}


/* init_sql 
 *
 * Initialize the in-memory SQL DB.
 *
 */
void
init_sql ()
{
  char *zErrMsg = 0;

  int rc = sqlite3_open (":memory:", &db);

  if (rc)
  {
    UT_LOG (Fatal, "Can't open database: %s\n", sqlite3_errmsg (db));
    sqlite3_close (db);
  }
  rc = sqlite3_exec (db, "BEGIN TRANSACTION; "
		     "CREATE TABLE items (class TEXT, id INTEGER PRIMARY KEY, value TEXT, timestamp NUMERIC);"
		     "CREATE INDEX classidx ON items(class ASC);"
		     "CREATE INDEX keyidx ON items(value ASC);"
		     "COMMIT;", 0, 0, &zErrMsg);
  if (rc != SQLITE_OK)
  {
    UT_LOG (Fatal, "SQL error: %s\n", zErrMsg);
    sqlite3_free (zErrMsg);
  }
}

/* config_error
 *
 * Handle configuration errors by logging and dying.
 *
 */

void
config_error ()
{
  UT_LOG (Fatal, "Config error in line %d: %s",
	  config_error_line (&conf), config_error_text (&conf));
}


/* init_config
 *
 * Parses configuration file and loads classes and keys into the 
 * proper data structures.
 *
 */

void
init_config ()
{
  config_init (&conf);
  if (CONFIG_FALSE == config_read_file (&conf, "config"))
  {
    config_error ();
  }

  // Read basic configuration options

  config_setting_t *t;

  if (t = config_lookup (&conf, "settings.db_path"))
  {
    db_path = config_setting_get_string (t);
  }

  if (t = config_lookup (&conf, "settings.log"))
  {
    log = config_setting_get_string (t);
  }

  if (t = config_lookup (&conf, "settings.log_level"))
  {
    log_level = config_setting_get_int (t);
  }

  if (t = config_lookup (&conf, "settings.address"))
  {
    address = config_setting_get_string (t);
  }

  if (t = config_lookup (&conf, "settings.port"))
  {
    port = config_setting_get_int (t);
  }

  if (t = config_lookup (&conf, "settings.control_address"))
  {
    control_address = config_setting_get_string (t);
  }

  if (t = config_lookup (&conf, "settings.control_port"))
  {
    control_port = config_setting_get_int (t);
  }

  if (t = config_lookup (&conf, "settings.expiration_timer"))
  {
    expiration_timer = config_setting_get_int (t);
  }

  if (t = config_lookup (&conf, "settings.max_age"))
  {
    max_age = config_setting_get_int (t);
  }

  // Use defaults if needed

  if (!db_path)
    db_path = memory_db;
  if (!log)
    log = dev_stderr;
  if (!log_level)
    log_level = 3;
  if (!control_address)
    control_address = loopback;
  if (!control_port)
    control_port = 1999;
  if (!address)
    address = loopback;
  if (!port)
    port = 1999;
  if (!expiration_timer)
    expiration_timer = 180;
  if (!max_age)
    expiration_timer = 90;

  UT_LOG (Info, "Database: %s", db_path);
  UT_LOG (Info, "Expire marks every %ld", expiration_timer);

  // get the limits group
  config_setting_t *limits = config_lookup (&conf, "limits");

  if (!limits)
    config_error ();

  int i = 0;

  for (;; i++)			// Iterate over limit classes
  {
    config_setting_t *cl = config_setting_get_elem (limits, i);

    if (!cl)
      break;
    char *cname = config_setting_name (cl);

    //TODO: remove arbitrary limit
    if (strlen (cname) > 49)
    {
      UT_LOG (Fatal, "Class name exceeds 50 characters: %s", cname);
    }
    class_t *cls = (class_t *) calloc (1, sizeof (class_t));
    cls->name = (char *) calloc (50, sizeof (char));
    strcpy (cls->name, cname);
    cls->keys = NULL;
    class_tmp = NULL;
    UT_LOG (Info, "class: %s", cname);
    LL_ADD (class_list, class_tmp, cls);
    int j = 0;

    for (;; j++)		// Iterate over limits for this class
    {
      // Read config key and load it in a struct
      config_setting_t *skey = config_setting_get_elem (cl, j);

      if (!skey)
	break;
      rkey_t *tmp, *key = (rkey_t *) calloc (1, sizeof (rkey_t));

      key->time = config_setting_get_int_elem (skey, 1);
      key->count = config_setting_get_int_elem (skey, 2);
      key->name = config_setting_get_string_elem (skey, 0);
      key->next = NULL;

      UT_LOG (Debug, "Loaded Key: %s %d/%d",key->name,key->count,key->time);

      // Then add it to the linked list for the class
      LL_ADD (cls->keys, tmp, key);
    }
  }

}

/* main
 * 
 * Initialize everything and enter the event loop.
 *
 */

int
main (int argc, char **argv)
{
  // Parse config file
  init_config ();

  // Initialize libut
  bstring listening = bformat ("%s:%ld", control_address, control_port);

  UT_init (INIT_SIGNALS (SIGINT, SIGQUIT, SIGTERM), INIT_SHL_IPPORT,
	   listening->data, INIT_LOGFILE, log, INIT_LOGLEVEL, log_level, INIT_END);
  bdestroy (listening);

  // Setup signal handler
  UT_signal_reg (signal_handler);

  // Setup SQL Db
  init_sql ();

  // Setup cleanup timer
  UT_tmr_set ("cleanup", 1000 * expiration_timer, clean_old_marks, NULL);

  // Start listening
  listening = bformat ("%s:%ld", address, port);
  UT_net_listen ("rater", listening->data, handle, NULL);
  bdestroy (listening);

  // Enter event loop
  UT_event_loop ();
}
