//
// FILE            iotajsonExecutor.c
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
// The IoT Agent JSON's commands, as Service Executions - see header.
//
// Threads: serviceExecute runs on a broker thread and never blocks - an MQTT command is published (the
// publish is queued by libmosquitto), an HTTP command is queued for the HTTP worker, a pull command is
// stored. Answers arrive on libmosquitto's thread (cmdexe), on the worker (the POST's response) and on the
// southbound server's thread (a device's getCmd=1 and POST /iot/json/commands); all report through
// brokerP->serviceUpdateIn - never from inside serviceExecute, on the broker's thread.
//
#include <pthread.h>                                  // pthread_*
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // malloc, free, atoi
#include <string.h>                                   // strlen, strncmp, strchr, strdup, memcpy
#include <strings.h>                                  // strcasecmp, strncasecmp

#include <mosquitto.h>                                // mosquitto_*

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup
#include "corRest/corRestClient.h"                    // corRestClient*

#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "iotajsonSouth.h"                            // iotajsonSouthOpen
#include "iotajsonMeasures.h"                         // iotajsonMeasure
#include "iotajsonExecutor.h"                         // Own interface



#define SCHEME_MQTT   "iota-json+mqtt://"
#define SCHEME_HTTP   "iota-json+http://"
#define SCHEME_HTTPS  "iota-json+https://"
#define SCHEME_POLL   "iota-json+poll://"



// -----------------------------------------------------------------------------
//
// Pending - a command waiting for the device's answer, oldest first per (apikey, device, cmd): an MQTT
// command published, or a pull command - stored until the device asks (getCmd=1), then delivered
//
typedef struct Pending
{
  char             executionId[128];
  char             apikey[128];
  char             deviceId[256];
  char             cmd[128];
  bool             pull;                              // iota-json+poll://
  bool             delivered;                         // a pull command handed to the device
  char*            payload;                           // a pull command's {"<cmd>": <value>}, malloc'd
  struct Pending*  next;
} Pending;

//
// HttpJob - a command to POST, for the worker
//
typedef struct HttpJob
{
  char*            url;                               // http[s]://..., without the cmd parameter
  char*            body;
  char*            cmd;
  char             executionId[128];
  struct HttpJob*  next;
} HttpJob;



static const BridgeBroker*  brokerP     = NULL;
static struct mosquitto*    mosq        = NULL;
static bool                 connected   = false;
static char                 serverDesc[300];

static pthread_mutex_t      pendingMutex = PTHREAD_MUTEX_INITIALIZER;
static Pending*             pendingHead  = NULL;

static pthread_mutex_t      jobMutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t       jobCond      = PTHREAD_COND_INITIALIZER;
static HttpJob*             jobHead      = NULL;
static HttpJob*             jobTail      = NULL;
static bool                 workerStop   = false;
static bool                 workerUp     = false;
static pthread_t            worker;
static bool                 southUp      = false;



// -----------------------------------------------------------------------------
//
// report - serviceUpdateIn, if the broker has it
//
static void reportProgress(const char* executionId, const char* status, const char* progressJson, const char* outputJson, const char* errorJson)
{
  if ((brokerP == NULL) || (brokerP->abiVersion < 11) || (brokerP->serviceUpdateIn == NULL))
    return;

  int r = brokerP->serviceUpdateIn("iotajson", executionId, status, progressJson, outputJson, errorJson);

  if ((r != BRIDGE_OK) && (r != BRIDGE_NOT_FOUND))
    COR_W("iotajson: the broker did not take the report on '%s' (%d)", executionId, r);
}

static void report(const char* executionId, const char* status, const char* outputJson, const char* errorJson)
{
  reportProgress(executionId, status, NULL, outputJson, errorJson);
}



// -----------------------------------------------------------------------------
//
// pendingFree -
//
static void pendingFree(Pending* p)
{
  free(p->payload);
  free(p);
}



// -----------------------------------------------------------------------------
//
// failed - an execution failed, with a ProblemDetails saying why
//
static void failed(const char* executionId, int status, const char* title, const char* detail)
{
  char      buf[2048];
  CorAlloc  ka;
  char      kaBuf[4096];

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 4096, NULL, "iotajson problem");

  CorNode* pdP = corTreeObject(&ka, NULL);

  corTreeChildAdd(pdP, corTreeString(&ka, "type",   "https://uri.etsi.org/ngsi-ld/errors/InternalError"));
  corTreeChildAdd(pdP, corTreeString(&ka, "title",  title));
  corTreeChildAdd(pdP, corTreeInteger(&ka, "status", status));
  corTreeChildAdd(pdP, corTreeString(&ka, "detail", detail));

  if (corJsonFastRenderSize(pdP) < (int) sizeof(buf))
  {
    corJsonFastRender(pdP, buf);
    report(executionId, "failed", NULL, buf);
  }
  else
    report(executionId, "failed", NULL, NULL);

  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// render - a tree as JSON text, malloc'd
//
static char* render(CorNode* nodeP)
{
  int   size = corJsonFastRenderSize(nodeP) + 1;
  char* text = malloc(size);

  if (text != NULL)
    corJsonFastRender(nodeP, text);

  return text;
}



// -----------------------------------------------------------------------------
//
// commandPayload - {"<cmd>": <input.value>} - or the input without id and type, when it has no "value" -
// malloc'd; NULL: the input is not a JSON object
//
static char* commandPayload(const char* cmd, const char* inputJson)
{
  CorAlloc  ka;
  CorJson   cj;
  char      kaBuf[16 * 1024];
  char*     copy = strdup((inputJson != NULL) ? inputJson : "{}");
  char*     out  = NULL;

  if (copy == NULL)
    return NULL;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "iotajson input");
  corJsonCreate(&cj, &ka);

  CorNode* inP = corJsonParse(&cj, copy);

  if ((inP != NULL) && (inP->type == CorObject))
  {
    CorNode* valueP = corTreeLookup(inP, "value");

    if (valueP == NULL)
    {
      CorNode* idP   = corTreeLookup(inP, "id");
      CorNode* typeP = corTreeLookup(inP, "type");

      if (idP   != NULL) corTreeChildRemove(inP, idP);
      if (typeP != NULL) corTreeChildRemove(inP, typeP);
      valueP = inP;
    }
    else
      corTreeChildRemove(inP, valueP);

    CorNode* payloadP = corTreeObject(&ka, NULL);

    valueP->name = (char*) cmd;
    corTreeChildAdd(payloadP, valueP);
    out = render(payloadP);
  }

  corAllocBufferReset(&ka, false);
  free(copy);
  return out;
}



// -----------------------------------------------------------------------------
//
// segment - the next '/'-separated part of s into buf; NULL at the end
//
static const char* segment(const char* s, char* buf, int size)
{
  int n = 0;

  while ((*s != 0) && (*s != '/') && (*s != '?'))
  {
    if (n < size - 1)
      buf[n++] = *s;
    s++;
  }
  buf[n] = 0;

  return (n == 0) ? NULL : s;
}



// -----------------------------------------------------------------------------
//
// queryParam - the value of 'name' in a query string (after '?'), into buf; false: not there
//
static bool queryParam(const char* query, const char* name, char* buf, int size)
{
  int nameLen = strlen(name);

  for (const char* p = query; (p != NULL) && (*p != 0); )
  {
    if ((strncmp(p, name, nameLen) == 0) && (p[nameLen] == '='))
    {
      const char* v   = &p[nameLen + 1];
      const char* end = strchr(v, '&');
      int         len = (end != NULL) ? (int) (end - v) : (int) strlen(v);

      if (len >= size)
        len = size - 1;
      memcpy(buf, v, len);
      buf[len] = 0;
      return true;
    }

    p = strchr(p, '&');
    p = (p != NULL) ? p + 1 : NULL;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// deviceParse - <apikey>/<deviceId>/<cmd> after the scheme, into pP; what follows it (NUL or '?'), NULL:
// not that
//
static const char* deviceParse(const char* url, const char* scheme, Pending* pP)
{
  const char* s = &url[strlen(scheme)];

  memset(pP, 0, sizeof(Pending));

  if (((s = segment(s, pP->apikey, sizeof(pP->apikey))) == NULL) || (*s++ != '/') ||
      ((s = segment(s, pP->deviceId, sizeof(pP->deviceId))) == NULL) || (*s++ != '/') ||
      ((s = segment(s, pP->cmd, sizeof(pP->cmd))) == NULL) || ((*s != 0) && (*s != '?')))
  {
    COR_W("iotajson: '%s' - not %s<apikey>/<deviceId>/<command>", url, scheme);
    return NULL;
  }

  return s;
}



// -----------------------------------------------------------------------------
//
// pendingAppend - the newest of its (apikey, device, cmd)
//
static void pendingAppend(Pending* pendingP)
{
  pthread_mutex_lock(&pendingMutex);
  Pending** lastPP = &pendingHead;
  while (*lastPP != NULL)
    lastPP = &(*lastPP)->next;
  *lastPP = pendingP;
  pthread_mutex_unlock(&pendingMutex);
}



// -----------------------------------------------------------------------------
//
// mqttExecute - iota-json+mqtt://<apikey>/<deviceId>/<cmd>[?qos=..&retain=..]
//
static int mqttExecute(const char* url, const char* executionId, const char* inputJson)
{
  Pending     p;
  const char* s = deviceParse(url, SCHEME_MQTT, &p);

  if (s == NULL)
    return BRIDGE_BAD_INPUT;

  char qosBuf[8]    = "0";
  char retainBuf[8] = "false";

  if (*s == '?')
  {
    queryParam(&s[1], "qos",    qosBuf,    sizeof(qosBuf));
    queryParam(&s[1], "retain", retainBuf, sizeof(retainBuf));
  }

  if (mosq == NULL)
  {
    COR_W("iotajson: '%s' - no MQTT server configured (\"server\": { \"uri\": ... })", url);
    return BRIDGE_ERR;
  }

  char* payload = commandPayload(p.cmd, inputJson);

  if (payload == NULL)
    return BRIDGE_BAD_INPUT;

  char topic[600];

  snprintf(topic, sizeof(topic), "/%s/%s/cmd", p.apikey, p.deviceId);
  snprintf(p.executionId, sizeof(p.executionId), "%s", executionId);

  //
  // Pending BEFORE the publish: the device may answer before mosquitto_publish returns
  //
  Pending* pendingP = malloc(sizeof(Pending));

  if (pendingP == NULL)
  {
    free(payload);
    return BRIDGE_ERR;
  }

  *pendingP = p;
  pendingAppend(pendingP);

  int rc = mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, atoi(qosBuf), strcmp(retainBuf, "true") == 0);

  free(payload);

  if (rc != MOSQ_ERR_SUCCESS)
  {
    pthread_mutex_lock(&pendingMutex);
    for (Pending** pp = &pendingHead; *pp != NULL; pp = &(*pp)->next)
    {
      if (*pp == pendingP)
      {
        *pp = pendingP->next;
        break;
      }
    }
    pthread_mutex_unlock(&pendingMutex);
    pendingFree(pendingP);

    COR_W("iotajson: command '%s' to %s: %s", p.cmd, topic, mosquitto_strerror(rc));
    return BRIDGE_ERR;
  }

  //
  // No report from here: this is the broker's thread, inside its serviceExecute call - the broker has stored
  // the execution as executing already; the outcome comes on libmosquitto's thread (cmdexe)
  //
  COR_V("iotajson: execution '%s': published on %s", executionId, topic);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// pollExecute - iota-json+poll://<apikey>/<deviceId>/<cmd> - stored until the device asks for its commands
// (getCmd=1 on the southbound port)
//
static int pollExecute(const char* url, const char* executionId, const char* inputJson)
{
  Pending p;

  if (deviceParse(url, SCHEME_POLL, &p) == NULL)
    return BRIDGE_BAD_INPUT;

  if (southUp == false)
  {
    COR_W("iotajson: '%s' - no southbound port configured (\"southbound\": { \"port\": ... }): a device cannot ask for it", url);
    return BRIDGE_ERR;
  }

  Pending* pendingP = malloc(sizeof(Pending));

  if (pendingP == NULL)
    return BRIDGE_ERR;

  *pendingP = p;
  pendingP->pull    = true;
  pendingP->payload = commandPayload(p.cmd, inputJson);
  snprintf(pendingP->executionId, sizeof(pendingP->executionId), "%s", executionId);

  if (pendingP->payload == NULL)
  {
    free(pendingP);
    return BRIDGE_BAD_INPUT;
  }

  pendingAppend(pendingP);

  COR_V("iotajson: execution '%s': stored for %s/%s", executionId, p.apikey, p.deviceId);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// httpExecute - iota-json+http[s]://<host>[:<port>]/<path>?cmd=<cmd>[&...] - queued for the worker
//
static int httpExecute(const char* url, const char* executionId, const char* inputJson)
{
  const char* rest  = strstr(url, "://") + 3;
  bool        tls   = (strncmp(url, SCHEME_HTTPS, strlen(SCHEME_HTTPS)) == 0);
  const char* query = strchr(rest, '?');
  char        cmd[128];

  if ((query == NULL) || (queryParam(&query[1], "cmd", cmd, sizeof(cmd)) == false) || (cmd[0] == 0))
  {
    COR_W("iotajson: '%s' - the command is missing (?cmd=<command>)", url);
    return BRIDGE_BAD_INPUT;
  }

  //
  // The device's URL: the scheme without "iota-json+", and the query without cmd=
  //
  int   size   = strlen(url) + 16;
  char* devUrl = malloc(size);

  if (devUrl == NULL)
    return BRIDGE_ERR;

  int at = snprintf(devUrl, size, "%s://%.*s", (tls == true) ? "https" : "http", (int) (query - rest), rest);
  char sep = '?';

  for (const char* p = &query[1]; (p != NULL) && (*p != 0); )
  {
    const char* end = strchr(p, '&');
    int         len = (end != NULL) ? (int) (end - p) : (int) strlen(p);

    if (strncmp(p, "cmd=", 4) != 0)
    {
      at += snprintf(&devUrl[at], size - at, "%c%.*s", sep, len, p);
      sep = '&';
    }

    p = (end != NULL) ? end + 1 : NULL;
  }

  char*    payload = commandPayload(cmd, inputJson);
  HttpJob* jobP    = calloc(1, sizeof(HttpJob));

  if ((payload == NULL) || (jobP == NULL))
  {
    free(devUrl);
    free(payload);
    free(jobP);
    return (payload == NULL) ? BRIDGE_BAD_INPUT : BRIDGE_ERR;
  }

  jobP->url  = devUrl;
  jobP->body = payload;
  jobP->cmd  = strdup(cmd);
  snprintf(jobP->executionId, sizeof(jobP->executionId), "%s", executionId);

  pthread_mutex_lock(&jobMutex);
  if (jobTail != NULL)
    jobTail->next = jobP;
  else
    jobHead = jobP;
  jobTail = jobP;
  pthread_cond_signal(&jobCond);
  pthread_mutex_unlock(&jobMutex);

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// httpAnswer - a device's 200/201 response: {"<cmd>": <result>} as iotagent-json reads it - the result is
// the execution's output; another body is the output as it is (JSON, else a string)
//
static void httpAnswer(HttpJob* jobP, CorRestClientResponse* respP)
{
  CorAlloc  ka;
  CorJson   cj;
  char      kaBuf[16 * 1024];
  char*     output = NULL;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "iotajson answer");
  corJsonCreate(&cj, &ka);

  if ((respP->body != NULL) && (respP->bodyLen > 0))
  {
    char* copy = corAlloc(&ka, respP->bodyLen + 1);   // the body by its length: it need not end in a NUL
    char* text = corAlloc(&ka, respP->bodyLen + 1);   // corJsonParse parses in place - a second copy

    if ((copy == NULL) || (text == NULL))
    {
      corAllocBufferReset(&ka, false);
      failed(jobP->executionId, 500, "Out Of Memory", "the device's answer could not be read");
      return;
    }

    memcpy(copy, respP->body, respP->bodyLen);
    memcpy(text, respP->body, respP->bodyLen);
    copy[respP->bodyLen] = 0;
    text[respP->bodyLen] = 0;

    CorNode* bodyP = corJsonParse(&cj, copy);

    if (bodyP != NULL)
    {
      CorNode* resultP = (bodyP->type == CorObject) ? corTreeLookup(bodyP, jobP->cmd) : NULL;

      if (resultP != NULL)
      {
        resultP->name = NULL;
        output = render(resultP);
      }
      else
        output = render(bodyP);
    }
    else
      output = render(corTreeString(&ka, NULL, text));
  }

  report(jobP->executionId, "completed", output, NULL);

  free(output);
  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// httpWorker - POSTs the queued commands, one at a time
//
static void* httpWorker(void* arg)
{
  (void) arg;

  while (true)
  {
    pthread_mutex_lock(&jobMutex);
    while ((jobHead == NULL) && (workerStop == false))
      pthread_cond_wait(&jobCond, &jobMutex);

    if (workerStop == true)
    {
      pthread_mutex_unlock(&jobMutex);
      return NULL;
    }

    HttpJob* jobP = jobHead;
    jobHead = jobP->next;
    if (jobHead == NULL)
      jobTail = NULL;
    pthread_mutex_unlock(&jobMutex);

    CorRestClientRequest  req;
    CorRestClientResponse resp;

    corRestClientRequestInit(&req, CorVerbPost, jobP->url, NULL);
    corRestClientRequestHeader(&req, "Content-Type", "application/json");
    corRestClientRequestBody(&req, jobP->body, strlen(jobP->body));
    corRestClientRequestTimeout(&req, 5000, 10000);

    int rc = corRestClientSend(&req, &resp);

    COR_V("iotajson: execution '%s': POST %s - %d", jobP->executionId, jobP->url, resp.statusCode);

    if ((rc == CORR_OK) && ((resp.statusCode == 200) || (resp.statusCode == 201)))
      httpAnswer(jobP, &resp);
    else
    {
      char detail[512];

      if ((rc != CORR_OK) || (resp.statusCode <= 0))
        snprintf(detail, sizeof(detail), "the command could not be sent to %s: %s", jobP->url, (resp.errorDetail[0] != 0) ? resp.errorDetail : "no answer");
      else
        snprintf(detail, sizeof(detail), "the device answered %d", resp.statusCode);

      failed(jobP->executionId, 502, "Device Refused", detail);
    }

    corRestClientResponseCleanup(&resp);
    corRestClientRequestCleanup(&req);

    free(jobP->url);
    free(jobP->body);
    free(jobP->cmd);
    free(jobP);
  }
}



// -----------------------------------------------------------------------------
//
// cmdexe - {"<cmd>": <result>, ...} from a device: each member completes the oldest execution of that
// command on that device
//
static void cmdexe(const char* apikey, const char* deviceId, const char* payload, int len)
{
  CorAlloc  ka;
  CorJson   cj;
  char      kaBuf[16 * 1024];

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "iotajson cmdexe");
  corJsonCreate(&cj, &ka);

  char* copy = malloc(len + 1);

  if (copy == NULL)
    return;
  memcpy(copy, payload, len);
  copy[len] = 0;

  CorNode* bodyP = corJsonParse(&cj, copy);

  if ((bodyP == NULL) || (bodyP->type != CorObject))
  {
    COR_W("iotajson: cmdexe from %s/%s is not a JSON object - dropped", apikey, deviceId);
    corAllocBufferReset(&ka, false);
    free(copy);
    return;
  }

  for (CorNode* mP = bodyP->value.head; mP != NULL; mP = mP->next)
  {
    Pending* foundP = NULL;

    pthread_mutex_lock(&pendingMutex);
    for (Pending** pp = &pendingHead; *pp != NULL; pp = &(*pp)->next)
    {
      Pending* p = *pp;

      if ((strcmp(p->apikey, apikey) == 0) && (strcmp(p->deviceId, deviceId) == 0) && (strcmp(p->cmd, mP->name) == 0))
      {
        *pp    = p->next;
        foundP = p;
        break;
      }
    }
    pthread_mutex_unlock(&pendingMutex);

    if (foundP == NULL)
    {
      COR_W("iotajson: cmdexe '%s' from %s/%s - no execution waits for it", mP->name, apikey, deviceId);
      continue;
    }

    const char* name = mP->name;

    mP->name = NULL;
    char* output = render(mP);
    mP->name = (char*) name;

    report(foundP->executionId, "completed", output, NULL);
    free(output);
    pendingFree(foundP);
  }

  corAllocBufferReset(&ka, false);
  free(copy);
}



// -----------------------------------------------------------------------------
//
// iotajsonCommandResult - a device's POST /iot/json/commands: as a cmdexe
//
void iotajsonCommandResult(const char* apikey, const char* deviceId, const char* body, int len)
{
  cmdexe(apikey, deviceId, body, len);
}



// -----------------------------------------------------------------------------
//
// iotajsonPullDeliver - the device's pull commands not yet delivered, in one object, as iotagent-json
// hands them over: {"<cmd>": <value>, ...} - a command stored twice, its later value; "{}": none. Each is
// delivered from here on: its execution still executing, its progress { "delivered": true }. malloc'd.
//
char* iotajsonPullDeliver(const char* apikey, const char* deviceId)
{
  Pending** deliveredV = NULL;
  int       delivered  = 0;
  int       size       = 3;

  pthread_mutex_lock(&pendingMutex);

  for (Pending* p = pendingHead; p != NULL; p = p->next)
  {
    if ((p->pull == true) && (p->delivered == false) && (strcmp(p->apikey, apikey) == 0) && (strcmp(p->deviceId, deviceId) == 0))
      delivered++;
  }

  if ((delivered > 0) && ((deliveredV = malloc(delivered * sizeof(Pending*))) == NULL))
  {
    pthread_mutex_unlock(&pendingMutex);
    return NULL;
  }

  delivered = 0;
  for (Pending* p = pendingHead; p != NULL; p = p->next)
  {
    if ((p->pull == true) && (p->delivered == false) && (strcmp(p->apikey, apikey) == 0) && (strcmp(p->deviceId, deviceId) == 0))
    {
      deliveredV[delivered++] = p;
      size += strlen(p->payload) + 1;
    }
  }

  //
  // {"a":1} {"b":2} -> {"a":1,"b":2}: each payload is an object of one member, rendered here (commandPayload)
  //
  char* out = malloc(size);
  int   at  = 0;

  if (out != NULL)
  {
    out[at++] = '{';

    for (int ix = 0; ix < delivered; ix++)
    {
      bool later = false;

      for (int jx = ix + 1; jx < delivered; jx++)
      {
        if (strcmp(deliveredV[jx]->cmd, deliveredV[ix]->cmd) == 0)
          later = true;
      }

      if (later == false)
      {
        int len = strlen(deliveredV[ix]->payload);

        if (at > 1)
          out[at++] = ',';
        memcpy(&out[at], &deliveredV[ix]->payload[1], len - 2);
        at += len - 2;
      }
    }

    out[at++] = '}';
    out[at]   = 0;
  }

  //
  // Delivered - and reported so after the lock: the ids copied, as the answer may end an execution meanwhile
  //
  char (*idV)[128] = (delivered > 0) ? malloc(delivered * 128) : NULL;

  for (int ix = 0; ix < delivered; ix++)
  {
    if (out != NULL)
      deliveredV[ix]->delivered = true;
    if (idV != NULL)
      snprintf(idV[ix], 128, "%s", deliveredV[ix]->executionId);
  }

  pthread_mutex_unlock(&pendingMutex);

  if ((out != NULL) && (idV != NULL))
  {
    for (int ix = 0; ix < delivered; ix++)
      reportProgress(idV[ix], "executing", "{\"delivered\":true}", NULL, NULL);
  }

  free(idV);
  free(deliveredV);
  return out;
}



// -----------------------------------------------------------------------------
//
// onMessage - libmosquitto's thread: [/][json/]<apikey>/<deviceId>/cmdexe, a command's answer, and
// [/][json/]<apikey>/<deviceId>/attrs[/<attribute>], a measure
//
// json/<a>/<b>/attrs is the "json" level's multi-measure, not a single measure of the device <b>/attrs:
// iotagent-json reads it so too.
//
static void onMessage(struct mosquitto* m, void* userdata, const struct mosquitto_message* msgP)
{
  (void) m;
  (void) userdata;

  const char* s = msgP->topic;
  char        level[5][256];
  int         levels = 0;

  if (*s == '/')
    s++;

  while ((*s != 0) && (levels < 5))
  {
    const char* next = segment(s, level[levels], sizeof(level[0]));

    if (next == NULL)
      return;
    levels++;
    s = (*next == '/') ? next + 1 : next;
  }

  if (*s != 0)
    return;

  int first = ((levels >= 4) && (strcasecmp(level[0], "json") == 0) && ((levels == 5) || (strcmp(level[3], "cmdexe") == 0) || (strcmp(level[3], "attrs") == 0))) ? 1 : 0;
  int rest  = levels - first;                         // <apikey>/<deviceId>/<what>[/<attribute>]

  if ((rest == 3) && (strcmp(level[first + 2], "cmdexe") == 0))
    cmdexe(level[first], level[first + 1], (const char*) msgP->payload, msgP->payloadlen);
  else if (((rest == 3) || (rest == 4)) && (strcmp(level[first + 2], "attrs") == 0))
  {
    const char* name    = NULL;
    char        message[512];
    const char* attrName = (rest == 4) ? level[first + 3] : NULL;
    int         status  = iotajsonMeasure(level[first], level[first + 1], (const char*) msgP->payload, msgP->payloadlen,
                                          attrName, IotaPayloadJson, NULL, &name, message, sizeof(message));

    if (status != 200)
      COR_W("iotajson: measure on '%s' dropped - %s: %s", msgP->topic, name, message);
  }
}



// -----------------------------------------------------------------------------
//
// onConnect / onDisconnect - libmosquitto's thread; it reconnects by itself
//
static void onConnect(struct mosquitto* m, void* userdata, int rc)
{
  (void) userdata;

  if (rc != 0)
  {
    COR_W("iotajson: MQTT server %s refused the connection (%d)", serverDesc, rc);
    return;
  }

  connected = true;

  //
  // The topics iotagent-json takes a command's answer on - with and without the "json" level
  //
  mosquitto_subscribe(m, NULL, "/+/+/cmdexe",     1);
  mosquitto_subscribe(m, NULL, "/json/+/+/cmdexe", 1);
  mosquitto_subscribe(m, NULL, "json/+/+/cmdexe",  1);

  //
  // ... and its measures: all at once, and one attribute at a time
  //
  mosquitto_subscribe(m, NULL, "/+/+/attrs",        1);
  mosquitto_subscribe(m, NULL, "/+/+/attrs/+",      1);
  mosquitto_subscribe(m, NULL, "/json/+/+/attrs",   1);
  mosquitto_subscribe(m, NULL, "/json/+/+/attrs/+", 1);
  mosquitto_subscribe(m, NULL, "json/+/+/attrs",    1);
  mosquitto_subscribe(m, NULL, "json/+/+/attrs/+",  1);

  COR_V("iotajson: connected to %s", serverDesc);
}

static void onDisconnect(struct mosquitto* m, void* userdata, int rc)
{
  (void) m;
  (void) userdata;

  connected = false;

  if (rc != 0)
    COR_W("iotajson: lost the MQTT server %s - reconnecting", serverDesc);
}



// -----------------------------------------------------------------------------
//
// mqttOpen - mqtt://host[:port]
//
static int mqttOpen(const char* uri)
{
  char        host[256];
  int         port = 1883;
  const char* s    = uri;

  if (strncmp(s, "mqtt://", 7) != 0)
  {
    COR_E("iotajson: server.uri '%s' - only mqtt://host[:port] for now", uri);
    return BRIDGE_ERR;
  }
  s += 7;

  const char* colon = strchr(s, ':');
  const char* slash = strchr(s, '/');
  int         len   = (colon != NULL) ? (int) (colon - s) : (slash != NULL) ? (int) (slash - s) : (int) strlen(s);

  if ((len <= 0) || (len >= (int) sizeof(host)))
  {
    COR_E("iotajson: server.uri '%s' - no host", uri);
    return BRIDGE_ERR;
  }
  memcpy(host, s, len);
  host[len] = 0;

  if (colon != NULL)
    port = atoi(&colon[1]);

  snprintf(serverDesc, sizeof(serverDesc), "%s:%d", host, port);

  mosquitto_lib_init();
  mosq = mosquitto_new(NULL, true, NULL);
  if (mosq == NULL)
    return BRIDGE_ERR;

  mosquitto_connect_callback_set(mosq, onConnect);
  mosquitto_disconnect_callback_set(mosq, onDisconnect);
  mosquitto_message_callback_set(mosq, onMessage);
  mosquitto_reconnect_delay_set(mosq, 1, 30, true);

  int rc = mosquitto_connect_async(mosq, host, port, 30);

  if ((rc != MOSQ_ERR_SUCCESS) && (rc != MOSQ_ERR_ERRNO))   // ERRNO: not up yet - the loop retries
  {
    COR_E("iotajson: cannot connect to %s: %s", serverDesc, mosquitto_strerror(rc));
    return BRIDGE_ERR;
  }

  if (mosquitto_loop_start(mosq) != MOSQ_ERR_SUCCESS)
  {
    COR_E("iotajson: cannot start libmosquitto's thread");
    return BRIDGE_ERR;
  }

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// iotajsonExecutorOpen -
//
int iotajsonExecutorOpen(CorNode* serverP, CorNode* southP, const BridgeBroker* _brokerP)
{
  brokerP = _brokerP;

  CorNode* uriP  = (serverP != NULL) ? corTreeLookup(serverP, "uri")  : NULL;
  CorNode* portP = (southP  != NULL) ? corTreeLookup(southP,  "port") : NULL;

  if ((uriP != NULL) && (uriP->type == CorString) && (mqttOpen(uriP->value.s) != BRIDGE_OK))
    return BRIDGE_ERR;

  if (portP != NULL)
  {
    if ((portP->type != CorInt) || (portP->value.i <= 0) || (portP->value.i > 65535))
    {
      COR_E("iotajson: southbound.port - a TCP port, 1-65535");
      return BRIDGE_ERR;
    }

    if (iotajsonSouthOpen((unsigned short) portP->value.i, brokerP) != BRIDGE_OK)
      return BRIDGE_ERR;
    southUp = true;
  }

  workerStop = false;
  if (pthread_create(&worker, NULL, httpWorker, NULL) != 0)
  {
    COR_E("iotajson: cannot start the HTTP worker");
    return BRIDGE_ERR;
  }
  workerUp = true;

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// iotajsonExecutorClose -
//
void iotajsonExecutorClose(void)
{
  if (workerUp == true)
  {
    pthread_mutex_lock(&jobMutex);
    workerStop = true;
    pthread_cond_signal(&jobCond);
    pthread_mutex_unlock(&jobMutex);
    pthread_join(worker, NULL);
    workerUp = false;
  }

  if (mosq != NULL)
  {
    mosquitto_disconnect(mosq);
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    mosq = NULL;
  }

  pthread_mutex_lock(&pendingMutex);
  while (pendingHead != NULL)
  {
    Pending* p = pendingHead;
    pendingHead = p->next;
    pendingFree(p);
  }
  pthread_mutex_unlock(&pendingMutex);

  brokerP = NULL;
}



// -----------------------------------------------------------------------------
//
// iotajsonServiceExecute - BridgeDriver.serviceExecute
//
int iotajsonServiceExecute(const char* url, const char* executionId, const char* inputJson)
{
  if (strncmp(url, SCHEME_MQTT, strlen(SCHEME_MQTT)) == 0)
    return mqttExecute(url, executionId, inputJson);

  if (strncmp(url, SCHEME_POLL, strlen(SCHEME_POLL)) == 0)
    return pollExecute(url, executionId, inputJson);

  if ((strncmp(url, SCHEME_HTTP, strlen(SCHEME_HTTP)) == 0) || (strncmp(url, SCHEME_HTTPS, strlen(SCHEME_HTTPS)) == 0))
    return httpExecute(url, executionId, inputJson);

  return BRIDGE_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// iotajsonServiceCancel - BridgeDriver.serviceCancel: a pull command the device has not asked for yet is
// withdrawn; a command handed to a device cannot be taken back
//
int iotajsonServiceCancel(const char* url, const char* executionId)
{
  (void) url;

  Pending* foundP    = NULL;
  bool     withdrawn = false;

  pthread_mutex_lock(&pendingMutex);
  for (Pending** pp = &pendingHead; *pp != NULL; pp = &(*pp)->next)
  {
    if (strcmp((*pp)->executionId, executionId) == 0)
    {
      foundP = *pp;

      if ((foundP->pull == true) && (foundP->delivered == false))
      {
        *pp       = foundP->next;
        withdrawn = true;
      }
      break;
    }
  }
  pthread_mutex_unlock(&pendingMutex);

  if (withdrawn == true)
  {
    pendingFree(foundP);
    return BRIDGE_OK;
  }

  return (foundP != NULL) ? BRIDGE_UNSUPPORTED : BRIDGE_NOT_FOUND;
}



// -----------------------------------------------------------------------------
//
// iotajsonExecutorVersion -
//
const char* iotajsonExecutorVersion(void)
{
  static char version[64];
  int         major, minor, revision;

  mosquitto_lib_version(&major, &minor, &revision);
  snprintf(version, sizeof(version), "libmosquitto %d.%d.%d", major, minor, revision);

  return version;
}
