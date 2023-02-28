/*
 * RealSimGear X-Plane Plugin
 * Copyright (C) 2022  Mario Haustein
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>
#include <lua.h>
#include <lauxlib.h>


struct device_t
{
  int fd;
};


static lua_State *L;
XPLMFlightLoopID loop_id;
static unsigned int ndevices = 0;
static struct device_t *devices = NULL;


static float loop_callback(
  __attribute__((unused)) float s_last_call,
  __attribute__((unused)) float s_last_loop,
  __attribute__((unused)) int count,
  __attribute__((unused)) void *ref)
{
  unsigned int i;
  char line[256];


  lua_checkstack(L, 3);
  lua_getfield(L, LUA_REGISTRYINDEX, "realsimgear");
  lua_getfield(L, -1, "devices");

  for(i = 0; i < ndevices; i++)
  {
    struct device_t *device;
    ssize_t result;
    char *saveptr;
    char *input;
    char *value;
    void (*action)(XPLMCommandRef);
    XPLMCommandRef cmdref;

    device = &devices[i];

    /* Skip invalid devices */
    if(device->fd < 0)
      continue;

    lua_pushinteger(L, i + 1);
    lua_gettable(L, -2);

    while(1)
    {
      result = read(device->fd, line, 256);

      /* Stop reading on errors */
      if(result < 0)
        break;

      /* Nothing read */
      if(result == 0)
        break;

      /* Ensure string termination */
      line[result] = '\0';

      /* Remove newline */
      if(line[result - 1] == '\n' || line[result - 1] == '\r')
        line[result - 1] = '\0';

      /* Skip Heartbeats */
      if(line[0] == '\0')
        continue;

      /* Ignore status messages */
      if(!isalpha(line[0]))
        continue;

      saveptr = NULL;
      input = strtok_r(line, "=", &saveptr);
      value = strtok_r(NULL, "=", &saveptr);

      if(value == NULL)
        action = XPLMCommandOnce;
      else if(value[0] == '0')
        action = XPLMCommandEnd;
      else if(value[0] == '1')
        action = XPLMCommandBegin;
      else
        continue;

      /* Resolve command */
      lua_getfield(L, -1, input);
      cmdref = lua_touserdata(L, -1);
      lua_pop(L, 1);

      if(cmdref == NULL)
        continue;

      /* Execute command */
      action(cmdref);
    }

    lua_pop(L, 1);
  }

  lua_settop(L, 0);

  return 0.1;
}

PLUGIN_API int XPluginStart(char *outName, char *outSig, char *outDesc)
{
  strcpy(outName, "RealSimGear");
  strcpy(outSig, "realsimgear.input");
  strcpy(outDesc, "Connect RealSimGear input devices to simulator");

  return 1;
}

PLUGIN_API void	XPluginStop()
{
}

PLUGIN_API int XPluginEnable()
{
  char debugstr[256];
  char path[PATH_MAX + 1];
  int result;
  XPLMCreateFlightLoop_t loop;
  unsigned int i;


  /* Ensure null termination. */
  debugstr[255] = '\0';


  /* Initialize Lua context */
  L = luaL_newstate();
  if(L == NULL)
  {
    XPLMDebugString("RealSimGear: Unable to set up Lua.\n");
    goto fail_lua;
  }


  /* Load configuration */
  XPLMGetSystemPath(path);
  strncat(path, "Resources/plugins/realsimgear/realsimgear.lua", PATH_MAX);

  result = luaL_dofile(L, path);
  if(result)
  {
    XPLMDebugString("RealSimGear: Error reading RealSimGear configuration.\n");
    XPLMDebugString(lua_tostring(L, -1));
    XPLMDebugString("\n");
    goto fail_loadconfig;
  }

  lua_settop(L, 0);

  /* Prepare Lua registry */
  lua_checkstack(L, 3);

  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, "realsimgear");

  lua_newtable(L);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "devices");

  /*
   * Stack Layout at this point
   *
   * 1: REGISTRY -> "realsimgear" -> "devices"
   * 0: REGISTRY -> "realsimgear"
   */

  /* Open devices */
  lua_checkstack(L, 1);
  lua_getglobal(L, "devices");

  if(!lua_istable(L, -1))
  {
    XPLMDebugString("RealSimGear: Device list `devices` must be defined an a Lua table.\n");
    goto fail_devices_table;
  }

  ndevices = lua_objlen(L, -1);
  if(ndevices == 0)
  {
    XPLMDebugString("RealSimGear: No devices configured.\n");
    goto fail_devices_count;
  }

  devices = calloc(ndevices, sizeof(struct device_t));
  if(devices == NULL)
  {
    XPLMDebugString("RealSimGear: Memory allocation error :-O.\n");
    goto fail_devices_memory;
  }

  /*
   * Stack Layout at this point
   *
   * 2: GLOBAL   -> "devices"
   * 1: REGISTRY -> "realsimgear" -> "devices"
   * 0: REGISTRY -> "realsimgear"
   */

  lua_checkstack(L, 3);
  for(i = 0; i < ndevices; i++)
  {
    struct device_t *device;
    const char *devicepath;
    struct termios termios;

    device = &devices[i];
    device->fd = -1;

    /* Add device table to registry */
    lua_newtable(L);
    lua_pushinteger(L, i + 1);
    lua_pushvalue(L, -2);
    lua_settable(L, -5);

    /* Get device table from configuration */
    lua_pushinteger(L, i + 1);
    lua_gettable(L, -3);
    if(!lua_istable(L, -1))
    {
      snprintf(debugstr, sizeof(debugstr) - 1, "RealSimGear: Device %d has an invalid Lua type. A table was expected.\n", i + 1);
      goto fail_dev_get;
    }

    lua_getfield(L, -1, "device");
    devicepath = lua_tostring(L, -1);
    if(devicepath == NULL)
    {
      snprintf(debugstr, sizeof(debugstr) - 1, "RealSimGear: Device %d has an invalid device path. A string was expected.\n", i + 1);
      lua_pop(L, 1);
      goto fail_dev_path;
    }

    /*
     * Stack Layout at this point
     *
     * 5: GLOBAL   -> "devices" -> [i] -> "device"
     * 4: GLOBAL   -> "devices" -> [i]
     * 3: REGISTRY -> "realsimgear" -> "devices" -> [i]
     * 2: GLOBAL   -> "devices"
     * 1: REGISTRY -> "realsimgear" -> "devices"
     * 0: REGISTRY -> "realsimgear
     */

    device->fd = open(devicepath, O_NONBLOCK);
    if(device->fd < 0)
    {
      snprintf(debugstr, sizeof(debugstr) - 1, "RealSimGear: Device %d: Unable to open %s: %s\n",
        i + 1, devicepath, strerror(errno));
      goto fail_dev_open;
    }

    result = tcgetattr(device->fd, &termios);
    if(result < 0)
    {
      snprintf(debugstr, sizeof(debugstr) - 1, "RealSimGear: Device %d: Unable to get terminal attributes: %s\n",
        i + 1, strerror(errno));
      goto fail_dev_term;
    }

    termios.c_lflag |= ICANON;
    termios.c_cflag |= (CLOCAL | CREAD);
    termios.c_cflag &= ~PARENB;
    termios.c_cflag &= ~CSTOPB;
    termios.c_cflag &= ~CSIZE;
    termios.c_cflag |= CS8;

    result = cfsetspeed(&termios, B115200);
    if(result < 0)
    {
      snprintf(debugstr, sizeof(debugstr) - 1, "RealSimGear: Device %d: Unable to set baud rate: %s\n",
        i + 1, strerror(errno));
      goto fail_dev_term;
    }

    result = tcsetattr(device->fd, TCSANOW, &termios);
    if(result < 0)
    {
      snprintf(debugstr, sizeof(debugstr) - 1, "RealSimGear: Device %d: Unable to set terminal attributes: %s\n",
        i + 1, strerror(errno));
      goto fail_dev_term;
    }

    lua_checkstack(L, 4);
    lua_getfield(L, -2, "mapping");
    lua_pushnil(L);
    while(lua_next(L, -2))
    {
      const char *command;
      XPLMCommandRef cmdref;

      /*
       * Stack Layout at this point
       *
       * 8: GLOBAL   -> "devices" -> [i] -> "mapping" -> value
       * 7: GLOBAL   -> "devices" -> [i] -> "mapping" -> key
       * 6: GLOBAL   -> "devices" -> [i] -> "mapping"
       * 5: GLOBAL   -> "devices" -> [i] -> "device"
       * 4: GLOBAL   -> "devices" -> [i]
       * 3: REGISTRY -> "realsimgear" -> "devices" -> [i]
       * 2: GLOBAL   -> "devices"
       * 1: REGISTRY -> "realsimgear" -> "devices"
       * 0: REGISTRY -> "realsimgear
       */

      /* Ignore invalid entries */
      if(!lua_isstring(L, -2) || !lua_isstring(L, -1))
      {
        lua_pop(L, 1);
        continue;
      }

      /* Resolve command */
      command = lua_tostring(L, -1);
      cmdref = XPLMFindCommand(command);
      lua_pop(L, 1);

      if(cmdref == NULL)
      {
        snprintf(debugstr, sizeof(debugstr) - 1, "RealSimGear: Device %d: Unkown command: %s\n",
          i + 1, command);
        XPLMDebugString(debugstr);
        continue;
      }

      lua_pushvalue(L, -1);
      lua_pushlightuserdata(L, cmdref);
      lua_settable(L, -7);
    }

    /*
     * Stack Layout at this point
     *
     * 6: GLOBAL   -> "devices" -> [i] -> "mapping"
     * 5: GLOBAL   -> "devices" -> [i] -> "device"
     * 4: GLOBAL   -> "devices" -> [i]
     * 3: REGISTRY -> "realsimgear" -> "devices" -> [i]
     * 2: GLOBAL   -> "devices"
     * 1: REGISTRY -> "realsimgear" -> "devices"
     * 0: REGISTRY -> "realsimgear
     */
    lua_pop(L, 4);

    /* Device sucessfully loaded */
    continue;


fail_dev_term:
    close(device->fd);

fail_dev_open:
fail_dev_path:
    lua_pop(L, 1);

fail_dev_get:
    lua_pop(L, 1);
    XPLMDebugString(debugstr);
  }

  lua_settop(L, 0);


  /* Register event loop */
  loop.structSize = sizeof(XPLMCreateFlightLoop_t);
  loop.phase = xplm_FlightLoop_Phase_AfterFlightModel;
  loop.callbackFunc = loop_callback;
  loop.refcon = NULL;

  loop_id = XPLMCreateFlightLoop(&loop);
  XPLMScheduleFlightLoop(loop_id, -1, 0);


  return 1;


fail_devices_memory:
fail_devices_count:
fail_devices_table:
fail_loadconfig:

  lua_close(L);
fail_lua:

  return 0;
}

PLUGIN_API void XPluginDisable()
{
  unsigned int i;


  XPLMDestroyFlightLoop(loop_id);

  for(i = 0; i < ndevices; i++)
  {
    struct device_t *device;

    device = &devices[i];

    if(device->fd >= 0)
      close(device->fd);
  }

  free(devices);
  devices = NULL;

  lua_close(L);
  L = NULL;
}

PLUGIN_API void XPluginReceiveMessage(
  __attribute__((unused)) XPLMPluginID inFromWho,
  __attribute__((unused)) int inMessage,
  __attribute__((unused)) void *inParam)
{
}
