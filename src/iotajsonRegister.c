//
// FILE            iotajsonRegister.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// The iotajson bridge - the SEAM: what the broker calls, and the one symbol it looks for (bridgeRegister).
// The contract is corBridge's BridgeDriver.h (broker -> plugin) and BridgeBroker.h (plugin -> broker).
//
// The IoT Agent JSON as a Service Executor (coraine doc/iot-agent-executor.md): a Service Registration
// whose endpoint is iota-json+mqtt:// or iota-json+http[s]:// is executed here - the device's command.
// Measures (Channels) are not built yet: a Channel is refused. The executor is in iotajsonExecutor.c.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // fopen, fread, fseek, ftell
#include <stdlib.h>                                   // malloc, free
#include <string.h>                                   // strdup
#include <strings.h>                                  // strcasecmp

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "iotajsonExecutor.h"                        // iotajsonExecutor*, iotajsonService*



// -----------------------------------------------------------------------------
//
// configLoad - the whole --bridgeConfig file, NUL-terminated; malloc'd. NULL: not readable
//
static char* configLoad(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// iotajsonInit - the MQTT connection, from the --bridgeConfig file's member of this bridge:
//
//   { "iotajson": { "server": { "uri": "mqtt://mosquitto:1883" } } }
//
// No "server": HTTP commands only.
//
static int iotajsonInit(const char* configFile, const BridgeBroker* brokerP)
{
  CorAlloc  ka;
  CorJson   cj;
  char      buf[16 * 1024];
  CorNode*  serverP = NULL;
  char*     text    = (configFile != NULL) ? configLoad(configFile) : NULL;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotajson config");
  corJsonCreate(&cj, &ka);

  if (text != NULL)
  {
    CorNode* rootP   = corJsonParse(&cj, text);       // valid JSON: the broker has parsed the same file
    CorNode* bridgeP = (rootP != NULL) ? corTreeLookup(rootP, "iotajson") : NULL;

    serverP = (bridgeP != NULL) ? corTreeLookup(bridgeP, "server") : NULL;
  }

  int r = iotajsonExecutorOpen(serverP, brokerP);

  corAllocBufferReset(&ka, false);                    // nothing parsed here is kept
  free(text);                                         // after the tree: its strings point into it
  return r;
}



// -----------------------------------------------------------------------------
//
// iotajsonClose - must not return until no thread of the plugin can call the broker
//
static void iotajsonClose(void)
{
  iotajsonExecutorClose();
}



// -----------------------------------------------------------------------------
//
// The Channels - measures are not built yet: every Channel is refused, saying so
//
static int iotajsonChannelAddInfo(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  (void) kind;
  (void) direction;
  (void) info;

  COR_W("iotajson: Channel '%s' refused - this bridge executes commands (Service Execution); measures are not built yet", endpoint);
  return BRIDGE_UNSUPPORTED;
}

static int iotajsonChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  return iotajsonChannelAddInfo(endpoint, kind, direction, NULL);
}

static int iotajsonChannelDel(const char* endpoint)
{
  (void) endpoint;
  return BRIDGE_NOT_FOUND;
}

static int iotajsonPublish(const char* endpoint, const char* json)
{
  (void) endpoint;
  (void) json;
  return BRIDGE_UNSUPPORTED;
}



// -----------------------------------------------------------------------------
//
// iotajsonVersionInfo - one line for GET /version: the plugin, and what it links
//
static const char* iotajsonVersionInfo(void)
{
  static char info[128];

  snprintf(info, sizeof(info), "iotajson 0.1.0 (%s)", iotajsonExecutorVersion());
  return info;
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for, and the one this .so exports
//
// A slot is filled only when the broker's abiVersion has it; abiVersion is then overwritten with this
// plugin's. Service Execution is ABI 11: an older broker loads the plugin, and it executes nothing.
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  if (driverP->abiVersion < 1)
    driverP->abiVersion = 1;                          // a host from before the handshake

  driverP->alias       = "iotajson";                  // --bridges iotajson, and its member of --bridgeConfig
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = iotajsonInit;
  driverP->close       = iotajsonClose;
  driverP->versionInfo = iotajsonVersionInfo;
  driverP->channelAdd  = iotajsonChannelAdd;
  driverP->channelDel  = iotajsonChannelDel;
  driverP->publish     = iotajsonPublish;

  if (driverP->abiVersion >= 9)
    driverP->channelAddInfo = iotajsonChannelAddInfo;

  if (driverP->abiVersion >= 11)
  {
    driverP->serviceSchemes = "iota-json+mqtt,iota-json+http,iota-json+https";
    driverP->serviceExecute = iotajsonServiceExecute;
    driverP->serviceCancel  = iotajsonServiceCancel;
  }

  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
