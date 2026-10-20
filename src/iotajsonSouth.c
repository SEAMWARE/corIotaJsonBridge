//
// FILE            iotajsonSouth.c
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
// The southbound port - see header. The broker serves it (BridgeBroker.httpListen, ABI 16); here only the
// routes: what iotagent-json's HTTP binding answers (lib/bindings/HTTPBinding.js), its errors
// {"name", "message"} included.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, realloc, free
#include <string.h>                                   // strlen, strcmp, strncmp, memcpy
#include <strings.h>                                  // strncasecmp

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V

#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BridgeHttpRequest, BRIDGE_*

#include "iotajsonExecutor.h"                         // iotajsonPullDeliver, iotajsonCommandResult
#include "iotajsonMeasures.h"                         // iotajsonMeasure, IotaPayload
#include "iotajsonSouth.h"                            // Own interface



// -----------------------------------------------------------------------------
//
// reply - a status and a JSON body, for the broker to send
//
static int reply(char** bodyP, int status, const char* json)
{
  *bodyP = strdup(json);
  return (*bodyP != NULL) ? status : 500;
}



// -----------------------------------------------------------------------------
//
// error - iotagent-json's error body: {"name": "<NAME>", "message": "<message>"}
//
static int error(char** bodyP, int status, const char* name, const char* message)
{
  char body[2048];

  snprintf(body, sizeof(body), "{\"name\":\"%s\",\"message\":\"%s\"}", name, message);
  return reply(bodyP, status, body);
}



// -----------------------------------------------------------------------------
//
// jsonEscape - a message as a JSON string's content: it names what a device sent
//
static const char* jsonEscape(const char* in, char* out, int size)
{
  int at = 0;

  for (; (*in != 0) && (at < size - 7); in++)
  {
    unsigned char c = (unsigned char) *in;

    if ((c == '"') || (c == '\\'))
    {
      out[at++] = '\\';
      out[at++] = c;
    }
    else if (c < 0x20)
      at += snprintf(&out[at], size - at, "\\u%04x", c);
    else
      out[at++] = c;
  }

  out[at] = 0;
  return out;
}



// -----------------------------------------------------------------------------
//
// serve - a request on the southbound port (BridgeHttpHandler)
//
static int serve(const BridgeHttpRequest* reqP, char** bodyP, const char** contentTypeP)
{
  const char* url    = reqP->path;
  const char* method = reqP->method;

  *contentTypeP = "application/json";

  bool        commands = (strcmp(url, "/iot/json/commands") == 0);
  const char* attrName = (strncmp(url, "/iot/json/attrs/", 16) == 0) ? &url[16] : NULL;

  if ((attrName != NULL) && ((attrName[0] == 0) || (strchr(attrName, '/') != NULL)))
    return error(bodyP, 404, "NOT_FOUND", "no such resource");

  if ((strcmp(url, "/iot/json") != 0) && (commands == false) && (attrName == NULL))
    return error(bodyP, 404, "NOT_FOUND", "no such resource");

  bool get  = (strcmp(method, "GET")  == 0);
  bool post = (strcmp(method, "POST") == 0);

  if (((get == false) && (post == false)) || ((commands == true || attrName != NULL) && (post == false)))
    return error(bodyP, 404, "NOT_FOUND", "no such resource");

  if (reqP->bodyTooBig != 0)
    return error(bodyP, 413, "PAYLOAD_TOO_LARGE", "request entity too large");

  const char* apikey   = reqP->param(reqP, "k");
  const char* deviceId = reqP->param(reqP, "i");
  const char* getCmd   = reqP->param(reqP, "getCmd");
  const char* d        = reqP->param(reqP, "d");
  bool        pull     = ((getCmd != NULL) && (strcmp(getCmd, "1") == 0));

  //
  // The mandatory parameters, as iotagent-json lists the missing ones - a GET carries its measure in d=,
  // unless it only asks for its commands
  //
  char missing[128] = "";
  int  at           = 0;

  if ((apikey == NULL) || (apikey[0] == 0))
    at += snprintf(&missing[at], sizeof(missing) - at, "%s\\\"API Key\\\"", (at > 0) ? "," : "");
  if ((deviceId == NULL) || (deviceId[0] == 0))
    at += snprintf(&missing[at], sizeof(missing) - at, "%s\\\"Device Id\\\"", (at > 0) ? "," : "");
  if ((get == true) && (d == NULL) && (pull == false))
    at += snprintf(&missing[at], sizeof(missing) - at, "%s\\\"Payload\\\"", (at > 0) ? "," : "");

  if (at > 0)
  {
    char message[256];

    snprintf(message, sizeof(message), "Some of the mandatory params weren't found in the request: [%s]", missing);
    return error(bodyP, 400, "MANDATORY_PARAMS_NOT_FOUND", message);
  }

  //
  // POST /iot/json/commands - the answers to commands; iotagent-json answers {} (or the pull commands)
  //
  if (commands == true)
  {
    if (reqP->bodyLen > 0)
      iotajsonCommandResult(apikey, deviceId, reqP->body, reqP->bodyLen);
  }
  else if (((get == true) && (d != NULL)) || ((post == true) && ((reqP->bodyLen > 0) || (attrName != NULL))))
  {
    //
    // A measure - in d= (GET), the body (POST), or one attribute's value (POST .../attrs/<name>), its
    // Content-Type saying what the value is
    //
    const char*  body    = (get == true) ? d : reqP->body;
    int          len     = (get == true) ? (int) strlen(d) : reqP->bodyLen;
    const char*  t       = reqP->param(reqP, "t");
    const char*  type    = reqP->header(reqP, "Content-Type");
    IotaPayload  payload = IotaPayloadJson;
    const char*  name    = NULL;
    char         message[512];
    char         escaped[1024];

    if ((type != NULL) && (strncasecmp(type, "text/plain", 10) == 0))
      payload = IotaPayloadText;
    else if ((type != NULL) && (strncasecmp(type, "application/octet-stream", 24) == 0))
      payload = IotaPayloadBinary;

    int status = iotajsonMeasure(apikey, deviceId, body, len, attrName, payload, t, &name, message, sizeof(message));

    if (status != 200)
    {
      COR_W("iotajson: a measure from %s/%s refused - %s: %s", apikey, deviceId, name, message);
      return error(bodyP, status, name, jsonEscape(message, escaped, sizeof(escaped)));
    }
  }

  if (pull == false)
    return reply(bodyP, 200, "{}");

  char* body = iotajsonPullDeliver(apikey, deviceId);

  if (body == NULL)
    return error(bodyP, 500, "INTERNAL_ERROR", "out of memory");

  COR_V("iotajson: %s/%s asked for its commands: %s", apikey, deviceId, body);

  *bodyP = body;                                      // malloc'd - the broker frees it
  return 200;
}



// -----------------------------------------------------------------------------
//
// iotajsonSouthOpen -
//
int iotajsonSouthOpen(unsigned short port, const BridgeBroker* brokerP)
{
  if ((brokerP->abiVersion < 16) || (brokerP->httpListen == NULL))
  {
    COR_E("iotajson: the southbound port %u needs a broker that serves it (bridge ABI 16, httpListen)", port);
    return BRIDGE_ERR;
  }

  if (brokerP->httpListen("iotajson", port, serve) != BRIDGE_OK)
  {
    COR_E("iotajson: cannot listen on the southbound port %u", port);
    return BRIDGE_ERR;
  }

  COR_V("iotajson: the southbound port %u", port);
  return BRIDGE_OK;
}
