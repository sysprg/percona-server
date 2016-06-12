/* Copyright (c) 2014 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>
#include <my_global.h>
#include <my_sys.h>
#include <my_list.h>
#include <my_pthread.h>
#include <my_atomic.h>
#include <typelib.h>
#include <limits.h>
#include <string.h>

static ulonglong starttime= 0;
static ulonglong concurrency= 0;
static ulonglong busytime_start= 0;
static ulonglong busytime= 0;
static ulonglong queries= 0;
static ulonglong totaltime= 0;

static my_atomic_rwlock_t sm_lock __attribute__((unused));

typedef enum { CTL_ON= 0, CTL_OFF= 1, CTL_RESET= 2 } sm_ctl_t;
static const char* sm_ctl_names[]= { "ON", "OFF", "RESET", NullS };
static TYPELIB sm_ctl_typelib= {
  array_elements(sm_ctl_names) - 1,
  "",
  sm_ctl_names,
  NULL
};

static
void sm_ctl_update(MYSQL_THD thd, struct st_mysql_sys_var *var,
                   void *var_ptr, const void *save);

static ulong sm_ctl= CTL_OFF;

static
MYSQL_THDVAR_BOOL(must_contribute,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT,
  "should this thread contribute?", NULL, NULL, FALSE);

static
MYSQL_THDVAR_ULONGLONG(start_time,
  PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT,
  "query start time in MS", NULL, NULL, 0, 0, ULONGLONG_MAX, 0);


static MYSQL_SYSVAR_ENUM(
  control,                           /* name */
  sm_ctl,                            /* var */
  PLUGIN_VAR_RQCMDARG,
  "Control the scalability metrics. Use this to turn ON/OFF or RESET metrics.",
  NULL,                              /* check func. */
  sm_ctl_update,                     /* update func. */
  CTL_OFF,                           /* default */
  &sm_ctl_typelib                    /* typelib */
);

static
void sm_reset()
{
  my_atomic_rwlock_wrlock(&sm_lock);
  my_atomic_store64(&starttime, my_micro_time() / 1000);
  my_atomic_store64(&busytime, 0ULL);
  my_atomic_store64(&queries, 0ULL);
  my_atomic_store64(&totaltime, 0ULL);
  my_atomic_rwlock_wrunlock(&sm_lock);
}


static
void sm_ctl_update(MYSQL_THD thd __attribute__((unused)),
                   struct st_mysql_sys_var *var __attribute__((unused)),
                   void *var_ptr __attribute__((unused)),
                   const void *save) {
  ulong new_val= *((sm_ctl_t*) save);

  sm_reset();

  if (new_val != CTL_RESET)
    sm_ctl= new_val;

}


static
int sm_plugin_init(void *arg __attribute__((unused)))
{
  my_atomic_rwlock_init(&sm_lock);

  sm_reset();

  return(0);
}


static
int sm_plugin_deinit(void *arg __attribute__((unused)))
{
  my_atomic_rwlock_destroy(&sm_lock);
  return(0);
}

static
void sm_query_started(MYSQL_THD thd)
{
  ulonglong zero= 0;
  ulonglong now= my_micro_time() / 1000;

  my_atomic_rwlock_wrlock(&sm_lock);
  if (my_atomic_cas64(&concurrency, &zero, 1ULL))
    busytime_start= now;
  else
    my_atomic_add64(&concurrency, 1ULL);
  my_atomic_rwlock_wrunlock(&sm_lock);

  THDVAR(thd, must_contribute)= TRUE;
  THDVAR(thd, start_time)= now;
}

static
void sm_query_finished(MYSQL_THD thd)
{
  ulonglong one= 1, now, start_time, save_busytime_start;

  if (!THDVAR(thd, must_contribute))
    return;

  now= my_micro_time() / 1000;
  start_time= THDVAR(thd, start_time);

  my_atomic_rwlock_wrlock(&sm_lock);
  save_busytime_start= my_atomic_load64(&busytime_start);
 
  if (my_atomic_cas64(&concurrency, &one, 0ULL))
    my_atomic_add64(&busytime, now - save_busytime_start);
  else
    my_atomic_add64(&concurrency, -1LL);

  my_atomic_add64(&totaltime, now - start_time);
  my_atomic_add64(&queries, 1ULL);
  my_atomic_rwlock_wrunlock(&sm_lock);
}

static
void sm_query_failed(MYSQL_THD thd)
{
  /* currently there is no difference between success and failure */
  sm_query_finished(thd);
}


static
int sm_elapsedtime(MYSQL_THD thd __attribute__((unused)),
                   struct st_mysql_show_var* var,
                   char *buff)
{
  ulonglong now;

  now= my_micro_time() / 1000;
  my_atomic_rwlock_wrlock(&sm_lock);
  *((ulonglong*)buff)= (sm_ctl == CTL_ON) ? now - my_atomic_load64(&starttime) : 0;
  my_atomic_rwlock_wrunlock(&sm_lock);
  var->type= SHOW_LONGLONG;
  var->value= buff;
  return(0);
}


static
int sm_queries(MYSQL_THD thd __attribute__((unused)),
               struct st_mysql_show_var* var,
               char *buff)
{
  ulonglong queries_local= 0;

  my_atomic_rwlock_rdlock(&sm_lock);
  queries_local= my_atomic_load64(&queries);
  my_atomic_rwlock_rdunlock(&sm_lock);

  *((ulonglong *) buff)= queries_local;
  var->type= SHOW_LONGLONG;
  var->value= buff;
  return(0);
}


static
int sm_totaltime(MYSQL_THD thd __attribute__((unused)),
                 struct st_mysql_show_var* var,
                 char *buff)
{
  ulonglong totaltime_local= 0;

  my_atomic_rwlock_rdlock(&sm_lock);
  totaltime_local= my_atomic_load64(&totaltime);
  my_atomic_rwlock_rdunlock(&sm_lock);

  *((ulonglong *) buff)= totaltime_local;
  var->type= SHOW_LONGLONG;
  var->value= buff;
  return(0);
}


static
int sm_busytime(MYSQL_THD thd __attribute__((unused)),
                struct st_mysql_show_var* var,
                char *buff)
{
  ulonglong busytime_start_local;
  ulonglong busytime_local;
  ulonglong now= 0;

  if (sm_ctl != CTL_OFF)
  {
    now= my_micro_time() / 1000;
    my_atomic_rwlock_rdlock(&sm_lock);
    busytime_start_local= my_atomic_load64(&busytime_start);
    busytime_local= my_atomic_load64(&busytime);
    my_atomic_rwlock_rdunlock(&sm_lock);

    *((ulonglong *) buff)= now - busytime_start_local + busytime_local;
  }
  else
  {
    *((ulonglong *) buff)= 0;
  }
  var->type= SHOW_LONGLONG;
  var->value= buff;
  return(0);
}


static void sm_notify(MYSQL_THD thd, unsigned int event_class,
                      const void *event)
{

  if (event_class == MYSQL_AUDIT_GENERAL_CLASS)
  {
    const struct mysql_event_general *event_general=
      (const struct mysql_event_general *) event;

    if (sm_ctl != CTL_ON)
    {
      return;
    }

    if (event_general->general_command &&
        event_general->event_subclass == MYSQL_AUDIT_GENERAL_LOG &&
        strcmp(event_general->general_command, "Query") == 0)
    {
      sm_query_started(thd);
    }
    else if (event_general->general_command &&
        event_general->event_subclass == MYSQL_AUDIT_GENERAL_LOG &&
        strcmp(event_general->general_command, "Execute") == 0)
    {
      sm_query_started(thd);
    }
    else if (event_general->general_query &&
        event_general->event_subclass == MYSQL_AUDIT_GENERAL_RESULT)
    {
      sm_query_finished(thd);
    }
    else if (event_general->general_query &&
        event_general->event_subclass == MYSQL_AUDIT_GENERAL_ERROR)
    {
      sm_query_failed(thd);
    }

  }
}

/*
 * Plugin system vars
 */
static struct st_mysql_sys_var* scalability_metrics_system_variables[] =
{
  MYSQL_SYSVAR(must_contribute),
  MYSQL_SYSVAR(start_time),
  MYSQL_SYSVAR(control),
  NULL
};

/*
  Plugin type-specific descriptor
*/
static struct st_mysql_audit scalability_metrics_descriptor=
{
  MYSQL_AUDIT_INTERFACE_VERSION,                    /* interface version    */
  NULL,                                             /* release_thd function */
  sm_notify,                                        /* notify function      */
  { MYSQL_AUDIT_GENERAL_CLASSMASK |
    MYSQL_AUDIT_CONNECTION_CLASSMASK }              /* class mask           */
};

/*
  Plugin status variables for SHOW STATUS
*/

static struct st_mysql_show_var simple_status[]=
{
  { "scalability_metrics_elapsedtime", (char *) &sm_elapsedtime, SHOW_FUNC },
  { "scalability_metrics_queries", (char *) &sm_queries, SHOW_FUNC },
  { "scalability_metrics_concurrency", (char *) &concurrency, SHOW_LONGLONG },
  { "scalability_metrics_totaltime", (char *) &sm_totaltime, SHOW_FUNC },
  { "scalability_metrics_busytime", (char *) &sm_busytime, SHOW_FUNC },
  { 0, 0, 0}
};


/*
  Plugin library descriptor
*/

mysql_declare_plugin(scalability_metrics)
{
  MYSQL_AUDIT_PLUGIN,                     /* type                            */
  &scalability_metrics_descriptor,        /* descriptor                      */
  "scalability_metrics",                  /* name                            */
  "Percona LLC and/or its affiliates",    /* author                          */
  "Scalability metrics",                  /* description                     */
  PLUGIN_LICENSE_GPL,
  sm_plugin_init,                         /* init function (when loaded)     */
  sm_plugin_deinit,                       /* deinit function (when unloaded) */
  0x0001,                                 /* version                         */
  simple_status,                          /* status variables                */
  scalability_metrics_system_variables,   /* system variables                */
  NULL,
  0,
}
mysql_declare_plugin_end;

