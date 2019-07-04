/*
   Copyright (c) 2011, 2019, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */


#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <mysql/plugin_auth.h>
#include "auth_pam_tool.h"
#include <my_global.h>

static char pam_debug = 0;
#define PAM_DEBUG(X)   do { if (pam_debug) { fprintf X; } } while(0)

static char winbind_hack = 0;

static char *opt_plugin_dir; /* To be dynamically linked. */
static const char *tool_name= "auth_pam_tool_dir/auth_pam_tool";
static const int tool_name_len= 31;

static int pam_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  int p_to_c[2], c_to_p[2]; /* Parent-to-child and child-to-parent pipes. */
  pid_t proc_id;
  int result= CR_ERROR, pkt_len;
  unsigned char field, *pkt;

  PAM_DEBUG((stderr, "PAM: opening pipes.\n"));
  if (pipe(p_to_c) < 0 || pipe(c_to_p) < 0)
  {
    /* Error creating pipes. */
    return CR_ERROR;
  }
  PAM_DEBUG((stderr, "PAM: forking.\n"));
  if ((proc_id= fork()) < 0)
  {
    /* Error forking. */
    close(p_to_c[0]);
    close(c_to_p[1]);
    goto error_ret;
  }

  if (proc_id == 0)
  {
    /* The 'sandbox' process started. */
    char toolpath[FN_REFLEN];
    size_t plugin_dir_len= strlen(opt_plugin_dir);

    PAM_DEBUG((stderr, "PAM: Child process prepares pipes.\n"));
    
    if (close(p_to_c[1]) < 0 ||
        close(c_to_p[0]) < 0 ||
        dup2(p_to_c[0], 0) < 0 || /* Parent's pipe to STDIN. */
        dup2(c_to_p[1], 1) < 0)   /* Sandbox's pipe to STDOUT. */
    {
      exit(-1);
    }

    PAM_DEBUG((stderr, "PAM: check tool directory: %s, %s.\n",
                       opt_plugin_dir, tool_name));
    if (plugin_dir_len + tool_name_len + 2 > sizeof(toolpath))
    {
      /* Tool path too long. */
      exit(-1);
    }

    memcpy(toolpath, opt_plugin_dir, plugin_dir_len);
    if (plugin_dir_len && toolpath[plugin_dir_len-1] != FN_LIBCHAR)
      toolpath[plugin_dir_len++]= FN_LIBCHAR;
    memcpy(toolpath+plugin_dir_len, tool_name, tool_name_len+1);

    PAM_DEBUG((stderr, "PAM: execute pam sandbox [%s].\n", toolpath));
    (void) execl(toolpath, toolpath, NULL);
    PAM_DEBUG((stderr, "PAM: exec() failed.\n"));
    exit(-1);
  }

  /* Parent process continues. */

  PAM_DEBUG((stderr, "PAM: parent continues.\n"));
  if (close(p_to_c[0]) < 0 ||
      close(c_to_p[1]) < 0)
    goto error_ret;

  /* no user name yet ? read the client handshake packet with the user name */
  if (info->user_name == 0)
  {
    if ((pkt_len= vio->read_packet(vio, &pkt)) < 0)
      return CR_ERROR;
  }
  else
    pkt= NULL;

  PAM_DEBUG((stderr, "PAM: parent sends user data [%s], [%s].\n",
               info->user_name, info->auth_string));

  field= pam_debug ? 1 : 0;
  field|= winbind_hack ? 2 : 0;

  if (write(p_to_c[1], &field, 1) != 1 ||
      write_string(p_to_c[1], (const uchar *) info->user_name,
                                       info->user_name_length) ||
      write_string(p_to_c[1], (const uchar *) info->auth_string,
                                      info->auth_string_length))
    goto error_ret;

  for (;;)
  {
    PAM_DEBUG((stderr, "PAM: listening to the sandbox.\n"));
    if (read(c_to_p[0], &field, 1) < 1)
    {
      PAM_DEBUG((stderr, "PAM: read failed.\n"));
      goto error_ret;
    }

    if (field == AP_EOF)
    {
      PAM_DEBUG((stderr, "PAM: auth OK returned.\n"));
      break;
    }

    switch (field)
    {
    case AP_AUTHENTICATED_AS:
      PAM_DEBUG((stderr, "PAM: reading authenticated_as string.\n"));
      if (read_string(c_to_p[0], info->authenticated_as,
                      sizeof(info->authenticated_as) - 1) < 0)
        goto error_ret;
      break;

    case AP_CONV:
      {
        unsigned char buf[10240];
        int buf_len;

        PAM_DEBUG((stderr, "PAM: getting CONV string.\n"));
        if ((buf_len= read_string(c_to_p[0], (char *) buf, sizeof(buf))) < 0)
          goto error_ret;

        if (!pkt || !*pkt || (buf[0] >> 1) != 2)
        {
          PAM_DEBUG((stderr, "PAM: sending CONV string.\n"));
          if (vio->write_packet(vio, buf, buf_len))
            goto error_ret;

          PAM_DEBUG((stderr, "PAM: reading CONV answer.\n"));
          if ((pkt_len= vio->read_packet(vio, &pkt)) < 0)
            goto error_ret;
        }

        PAM_DEBUG((stderr, "PAM: answering CONV.\n"));
        if (write_string(p_to_c[1], pkt, pkt_len))
          goto error_ret;

        pkt= NULL;
      }
      break;

    default:
      PAM_DEBUG((stderr, "PAM: unknown sandbox field.\n"));
      goto error_ret;
    }
  }
  result= CR_OK;

error_ret:
  close(p_to_c[1]);
  close(c_to_p[0]);
  waitpid(proc_id, NULL, WNOHANG);

  PAM_DEBUG((stderr, "PAM: auth result %d.\n", result));
  return result;
}


#include "auth_pam_common.c"


static int init(void *p __attribute__((unused)))
{
  if (use_cleartext_plugin)
    info.client_auth_plugin= "mysql_clear_password";
  if (!(opt_plugin_dir= dlsym(RTLD_DEFAULT, "opt_plugin_dir")))
    return 1;
  return 0;
}

maria_declare_plugin(pam)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &info,
  "pam",
  "MariaDB Corp",
  "PAM based authentication",
  PLUGIN_LICENSE_GPL,
  init,
  NULL,
  0x0200,
  NULL,
  vars,
  "2.0",
  MariaDB_PLUGIN_MATURITY_GAMMA
}
maria_declare_plugin_end;
