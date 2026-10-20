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
// The southbound port - see header. libmicrohttpd parses HTTP; here only the routes: what iotagent-json's
// HTTP binding answers (lib/bindings/HTTPBinding.js), its errors {"name", "message"} included.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, realloc, free
#include <string.h>                                   // strlen, strcmp, strncmp, memcpy
#include <strings.h>                                  // strncasecmp

#include <microhttpd.h>                               // MHD_*

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V

#include "corBridge/BridgeBroker.h"                   // BRIDGE_*

#include "iotajsonExecutor.h"                         // iotajsonPullDeliver, iotajsonCommandResult
#include "iotajsonMeasures.h"                         // iotajsonMeasure, IotaPayload
#include "iotajsonSouth.h"                            // Own interface



//
// A body's limit: iotagent-json's expressLimit default, 1 MB
//
#define BODY_MAX  (1024 * 1024)



// -----------------------------------------------------------------------------
//
// Upload - a request's body, read in parts
//
typedef struct Upload
{
  char*   buf;
  size_t  len;
  bool    tooBig;
} Upload;



static struct MHD_Daemon* daemonP = NULL;



// -----------------------------------------------------------------------------
//
// reply - a status and a JSON body
//
static enum MHD_Result reply(struct MHD_Connection* connection, unsigned int status, const char* json)
{
  struct MHD_Response* responseP = MHD_create_response_from_buffer(strlen(json), (void*) json, MHD_RESPMEM_MUST_COPY);

  if (responseP == NULL)
    return MHD_NO;

  MHD_add_response_header(responseP, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");

  enum MHD_Result r = MHD_queue_response(connection, status, responseP);

  MHD_destroy_response(responseP);
  return r;
}



// -----------------------------------------------------------------------------
//
// error - iotagent-json's error body: {"name": "<NAME>", "message": "<message>"}
//
static enum MHD_Result error(struct MHD_Connection* connection, unsigned int status, const char* name, const char* message)
{
  char body[2048];

  snprintf(body, sizeof(body), "{\"name\":\"%s\",\"message\":\"%s\"}", name, message);
  return reply(connection, status, body);
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
// serve - a request, its body read
//
static enum MHD_Result serve(struct MHD_Connection* connection, const char* url, const char* method, Upload* upP)
{
  bool        commands = (strcmp(url, "/iot/json/commands") == 0);
  const char* attrName = (strncmp(url, "/iot/json/attrs/", 16) == 0) ? &url[16] : NULL;

  if ((attrName != NULL) && ((attrName[0] == 0) || (strchr(attrName, '/') != NULL)))
    return error(connection, MHD_HTTP_NOT_FOUND, "NOT_FOUND", "no such resource");

  if ((strcmp(url, "/iot/json") != 0) && (commands == false) && (attrName == NULL))
    return error(connection, MHD_HTTP_NOT_FOUND, "NOT_FOUND", "no such resource");

  bool get  = (strcmp(method, MHD_HTTP_METHOD_GET)  == 0);
  bool post = (strcmp(method, MHD_HTTP_METHOD_POST) == 0);

  if (((get == false) && (post == false)) || ((commands == true || attrName != NULL) && (post == false)))
    return error(connection, MHD_HTTP_NOT_FOUND, "NOT_FOUND", "no such resource");

  if (upP->tooBig == true)
    return error(connection, MHD_HTTP_CONTENT_TOO_LARGE, "PAYLOAD_TOO_LARGE", "request entity too large");

  const char* apikey   = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "k");
  const char* deviceId = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "i");
  const char* getCmd   = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "getCmd");
  const char* d        = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "d");
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
    return error(connection, MHD_HTTP_BAD_REQUEST, "MANDATORY_PARAMS_NOT_FOUND", message);
  }

  //
  // POST /iot/json/commands - the answers to commands; iotagent-json answers {} (or the pull commands)
  //
  if (commands == true)
  {
    if (upP->len > 0)
      iotajsonCommandResult(apikey, deviceId, upP->buf, (int) upP->len);
  }
  else if (((get == true) && (d != NULL)) || ((post == true) && ((upP->len > 0) || (attrName != NULL))))
  {
    //
    // A measure - in d= (GET), the body (POST), or one attribute's value (POST .../attrs/<name>), its
    // Content-Type saying what the value is
    //
    const char*  body    = (get == true) ? d : ((upP->buf != NULL) ? upP->buf : "");
    int          len     = (get == true) ? (int) strlen(d) : (int) upP->len;
    const char*  t       = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "t");
    const char*  type    = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
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
      return error(connection, status, name, jsonEscape(message, escaped, sizeof(escaped)));
    }
  }

  if (pull == false)
    return reply(connection, MHD_HTTP_OK, "{}");

  char* body = iotajsonPullDeliver(apikey, deviceId);

  if (body == NULL)
    return error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "INTERNAL_ERROR", "out of memory");

  COR_V("iotajson: %s/%s asked for its commands: %s", apikey, deviceId, body);

  enum MHD_Result r = reply(connection, MHD_HTTP_OK, body);

  free(body);
  return r;
}



// -----------------------------------------------------------------------------
//
// answer - MHD's access handler: called first with no body (a new request), then once per part of the
// body, then with none left - the request served
//
static enum MHD_Result answer
(
  void*                   cls,
  struct MHD_Connection*  connection,
  const char*             url,
  const char*             method,
  const char*             version,
  const char*             uploadData,
  size_t*                 uploadDataSize,
  void**                  conClsP
)
{
  (void) cls;
  (void) version;

  Upload* upP = *conClsP;

  if (upP == NULL)
  {
    if ((upP = calloc(1, sizeof(Upload))) == NULL)
      return MHD_NO;

    *conClsP = upP;
    return MHD_YES;
  }

  if (*uploadDataSize > 0)
  {
    if ((upP->tooBig == false) && (upP->len + *uploadDataSize <= BODY_MAX))
    {
      char* bufP = realloc(upP->buf, upP->len + *uploadDataSize + 1);

      if (bufP == NULL)
        return MHD_NO;

      memcpy(&bufP[upP->len], uploadData, *uploadDataSize);
      upP->buf          = bufP;
      upP->len         += *uploadDataSize;
      upP->buf[upP->len] = 0;
    }
    else
      upP->tooBig = true;

    *uploadDataSize = 0;
    return MHD_YES;
  }

  return serve(connection, url, method, upP);
}



// -----------------------------------------------------------------------------
//
// completed - a request's end, served or not: its body freed
//
static void completed(void* cls, struct MHD_Connection* connection, void** conClsP, enum MHD_RequestTerminationCode toe)
{
  (void) cls;
  (void) connection;
  (void) toe;

  Upload* upP = *conClsP;

  if (upP != NULL)
  {
    free(upP->buf);
    free(upP);
    *conClsP = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// iotajsonSouthOpen -
//
int iotajsonSouthOpen(unsigned short port)
{
  daemonP = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_DUAL_STACK | MHD_USE_ERROR_LOG,
                             port, NULL, NULL,
                             answer, NULL,
                             MHD_OPTION_NOTIFY_COMPLETED, completed, NULL,
                             MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int) 30,
                             MHD_OPTION_END);

  if (daemonP == NULL)
  {
    COR_E("iotajson: cannot listen on the southbound port %u", port);
    return BRIDGE_ERR;
  }

  COR_V("iotajson: the southbound port %u", port);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// iotajsonSouthClose -
//
void iotajsonSouthClose(void)
{
  if (daemonP != NULL)
  {
    MHD_stop_daemon(daemonP);
    daemonP = NULL;
  }
}
