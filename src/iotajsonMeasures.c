//
// FILE            iotajsonMeasures.c
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
// The IoT Agent JSON's measures as DataChannel records - see iotajsonMeasures.h, and coraine's
// doc/iot-agent-measures.md § 2 for what a record is and where it goes.
//
#include <pthread.h>                                  // pthread_mutex_*
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // snprintf, sscanf
#include <stdlib.h>                                   // malloc, realloc, free, qsort
#include <string.h>                                   // strcmp, strlen, strdup, memcpy
#include <time.h>                                     // struct tm, timegm, clock_gettime

#include "corLog/corLog.h"                            // COR_W, COR_V
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
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "iotajsonMeasures.h"                         // Own interface



static const BridgeBroker*  brokerP        = NULL;
static pthread_mutex_t      endpointMutex  = PTHREAD_MUTEX_INITIALIZER;
static char**               endpointV      = NULL;   // the DataChannels' endpoints: <apikey>, <apikey>/<deviceId>
static int                  endpoints      = 0;



// -----------------------------------------------------------------------------
//
// iotajsonMeasuresOpen / Close -
//
void iotajsonMeasuresOpen(const BridgeBroker* _brokerP)
{
  brokerP = _brokerP;
}

void iotajsonMeasuresClose(void)
{
  pthread_mutex_lock(&endpointMutex);
  for (int ix = 0; ix < endpoints; ix++)
    free(endpointV[ix]);
  free(endpointV);
  endpointV = NULL;
  endpoints = 0;
  pthread_mutex_unlock(&endpointMutex);

  brokerP = NULL;
}



// -----------------------------------------------------------------------------
//
// iotajsonMeasureEndpointAdd / Del -
//
int iotajsonMeasureEndpointAdd(const char* endpoint)
{
  if ((endpoint == NULL) || (endpoint[0] == 0) || (endpoint[0] == '/'))
    return BRIDGE_BAD_INPUT;

  const char* slash = strchr(endpoint, '/');

  if ((slash != NULL) && ((slash[1] == 0) || (strchr(slash + 1, '/') != NULL)))
    return BRIDGE_BAD_INPUT;                          // <apikey> or <apikey>/<deviceId> - nothing else

  int rc = BRIDGE_OK;

  pthread_mutex_lock(&endpointMutex);

  bool known = false;

  for (int ix = 0; ix < endpoints; ix++)
  {
    if (strcmp(endpointV[ix], endpoint) == 0)
      known = true;
  }

  if (known == false)
  {
    char** v = realloc(endpointV, (endpoints + 1) * sizeof(char*));

    if ((v == NULL) || ((v[endpoints] = strdup(endpoint)) == NULL))
      rc = BRIDGE_ERR;
    else
      endpoints++;

    if (v != NULL)
      endpointV = v;
  }

  pthread_mutex_unlock(&endpointMutex);
  return rc;
}

int iotajsonMeasureEndpointDel(const char* endpoint)
{
  int rc = BRIDGE_NOT_FOUND;

  pthread_mutex_lock(&endpointMutex);
  for (int ix = 0; ix < endpoints; ix++)
  {
    if (strcmp(endpointV[ix], endpoint) == 0)
    {
      free(endpointV[ix]);
      endpointV[ix] = endpointV[--endpoints];
      rc = BRIDGE_OK;
      break;
    }
  }
  pthread_mutex_unlock(&endpointMutex);

  return rc;
}



// -----------------------------------------------------------------------------
//
// endpointOf - the DataChannel a device's measure goes to: its own, else its group's. NULL: none - and then
// *groupKnownP says whether the apikey has DataChannels of devices (DEVICE_NOT_FOUND) or none (no group)
//
static char* endpointOf(const char* apikey, const char* deviceId, bool* groupKnownP)
{
  char  device[512];
  char* found      = NULL;
  int   apikeyLen  = strlen(apikey);

  snprintf(device, sizeof(device), "%s/%s", apikey, deviceId);
  *groupKnownP = false;

  pthread_mutex_lock(&endpointMutex);

  for (int ix = 0; (ix < endpoints) && (found == NULL); ix++)
  {
    if (strcmp(endpointV[ix], device) == 0)
      found = strdup(device);
  }

  for (int ix = 0; (ix < endpoints) && (found == NULL); ix++)
  {
    if (strcmp(endpointV[ix], apikey) == 0)
      found = strdup(apikey);
    else if ((strncmp(endpointV[ix], apikey, apikeyLen) == 0) && (endpointV[ix][apikeyLen] == '/'))
      *groupKnownP = true;
  }

  pthread_mutex_unlock(&endpointMutex);
  return found;
}



// -----------------------------------------------------------------------------
//
// isoParse - ISO 8601, YYYY-MM-DDThh:mm:ss[.frac](Z|+hh:mm|-hh:mm), as epoch nanoseconds; false: it is none
//
static bool isoParse(const char* s, int64_t* nsP)
{
  struct tm tm;
  int       n = 0;

  memset(&tm, 0, sizeof(tm));

  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &n) != 6)
    return false;

  if ((n != 19) || (tm.tm_mon < 1) || (tm.tm_mon > 12) || (tm.tm_mday < 1) || (tm.tm_mday > 31) ||
      (tm.tm_hour > 23) || (tm.tm_min > 59) || (tm.tm_sec > 60))
    return false;

  const char* cP   = &s[n];
  int64_t     frac = 0;
  int         digits = 0;

  if (*cP == '.')
  {
    for (cP++; (*cP >= '0') && (*cP <= '9'); cP++)
    {
      if (digits < 9)
      {
        frac = frac * 10 + (*cP - '0');
        digits++;
      }
    }

    if (digits == 0)
      return false;

    for (; digits < 9; digits++)
      frac *= 10;
  }

  int offset = 0;                                     // seconds east of UTC

  if (*cP == 'Z')
    cP++;
  else if ((*cP == '+') || (*cP == '-'))
  {
    int hh, mm;

    if ((sscanf(cP + 1, "%2d:%2d", &hh, &mm) != 2) || (strlen(cP) != 6) || (hh > 23) || (mm > 59))
      return false;

    offset = ((*cP == '+') ? 1 : -1) * (hh * 3600 + mm * 60);
    cP    += 6;
  }
  else
    return false;

  if (*cP != 0)
    return false;

  tm.tm_year -= 1900;
  tm.tm_mon  -= 1;

  time_t secs = timegm(&tm);

  *nsP = ((int64_t) secs - offset) * 1000000000LL + frac;
  return true;
}



// -----------------------------------------------------------------------------
//
// nowNs - the arrival time
//
static int64_t nowNs(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}



// -----------------------------------------------------------------------------
//
// Record - one record of a measure, and its time
//
typedef struct Record
{
  CorNode*  nodeP;
  int64_t   time;
  int       ix;                                       // its position - equal times keep the device's order
} Record;

static int recordCompare(const void* a, const void* b)
{
  const Record* rA = a;
  const Record* rB = b;

  if (rA->time != rB->time)
    return (rA->time < rB->time) ? -1 : 1;

  return rA->ix - rB->ix;
}



// -----------------------------------------------------------------------------
//
// recordMake - a measure object as the record the DataChannel maps: the device's keys, 'id' and 'type'
// renamed measure_id / measure_type (as iotagent-json does), TimeInstant taken out as the record's time,
// and @deviceId / @apikey added
//
static CorNode* recordMake(CorAlloc* kaP, CorNode* measureP, const char* apikey, const char* deviceId, int64_t defaultTime, int64_t* timeP)
{
  CorNode* recordP = corTreeObject(kaP, NULL);

  *timeP = defaultTime;

  for (CorNode* memberP = measureP->value.head; memberP != NULL; memberP = memberP->next)
  {
    if (strcmp(memberP->name, "TimeInstant") == 0)
    {
      int64_t t;

      if ((memberP->type == CorString) && (isoParse(memberP->value.s, &t) == true))
      {
        *timeP = t;
        continue;
      }
    }

    CorNode* copyP = corTreeClone(kaP, memberP);

    if (copyP == NULL)
      return NULL;

    if (strcmp(memberP->name, "id") == 0)
      copyP->name = "measure_id";
    else if (strcmp(memberP->name, "type") == 0)
      copyP->name = "measure_type";

    corTreeChildAdd(recordP, copyP);
  }

  corTreeChildAdd(recordP, corTreeString(kaP, "@deviceId", deviceId));
  corTreeChildAdd(recordP, corTreeString(kaP, "@apikey",   apikey));

  return recordP;
}



// -----------------------------------------------------------------------------
//
// hexOf - bytes as hex text, as iotagent-json stores a binary measure
//
static char* hexOf(CorAlloc* kaP, const char* bytes, int len)
{
  char* hex = corAlloc(kaP, len * 2 + 1);

  if (hex == NULL)
    return NULL;

  for (int ix = 0; ix < len; ix++)
    snprintf(&hex[ix * 2], 3, "%02x", (unsigned char) bytes[ix]);

  hex[len * 2] = 0;
  return hex;
}



// -----------------------------------------------------------------------------
//
// singleValue - the value of a single measure (.../attrs/<name>)
//
static CorNode* singleValue(CorAlloc* kaP, CorJson* cjP, const char* body, int len, IotaPayload payload, const char* name)
{
  if (payload == IotaPayloadBinary)
    return corTreeString(kaP, name, hexOf(kaP, body, len));

  char* text = corAlloc(kaP, len + 1);

  if (text == NULL)
    return NULL;

  memcpy(text, body, len);
  text[len] = 0;

  if (payload == IotaPayloadText)
    return corTreeString(kaP, name, text);

  char* copy = corAlloc(kaP, len + 1);               // the parser writes into its buffer

  if (copy == NULL)
    return NULL;

  memcpy(copy, text, len + 1);

  CorNode* valueP = corJsonParse(cjP, copy);

  if (valueP != NULL)
  {
    valueP->name = (char*) name;
    return valueP;
  }

  return corTreeString(kaP, name, hexOf(kaP, body, len));   // not JSON: bytes, as MQTT's binary
}



// -----------------------------------------------------------------------------
//
// recordSend - a record to the broker, on the DataChannel's endpoint
//
static int recordSend(const char* endpoint, CorNode* recordP, int64_t time)
{
  int   size = corJsonFastRenderSize(recordP);
  char* json = malloc(size + 1);

  if (json == NULL)
    return BRIDGE_ERR;

  corJsonFastRender(recordP, json);

  int r = ((brokerP != NULL) && (brokerP->sampleIn != NULL)) ? brokerP->sampleIn("iotajson", endpoint, json, time) : BRIDGE_ERR;

  COR_V("iotajson: record on %s (%d): %s", endpoint, r, json);
  free(json);
  return r;
}



// -----------------------------------------------------------------------------
//
// iotajsonMeasure -
//
int iotajsonMeasure
(
  const char*   apikey,
  const char*   deviceId,
  const char*   body,
  int           len,
  const char*   attrName,
  IotaPayload   payload,
  const char*   timeParam,
  const char**  nameP,
  char*         message,
  int           messageSize
)
{
  bool  groupKnown;
  char* endpoint = endpointOf(apikey, deviceId, &groupKnown);

  if (endpoint == NULL)
  {
    if (groupKnown == true)
    {
      *nameP = "DEVICE_NOT_FOUND";
      snprintf(message, messageSize, "No device was found with id:%s and apikey %s", deviceId, apikey);
    }
    else
    {
      *nameP = "DEVICE_GROUP_NOT_FOUND";
      snprintf(message, messageSize, "Couldn't find device group for apikey %s", apikey);
    }

    return 404;
  }

  //
  // The time a record has when it says none: the 't' parameter, else now
  //
  int64_t defaultTime;

  if ((timeParam == NULL) || (isoParse(timeParam, &defaultTime) == false))
    defaultTime = nowNs();

  CorAlloc ka;
  CorJson  cj;
  char     buf[8 * 1024];

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotajson measure");
  corJsonCreate(&cj, &ka);

  int     status = 200;
  CorNode* measureP;

  if (attrName != NULL)
  {
    measureP = corTreeObject(&ka, NULL);
    corTreeChildAdd(measureP, singleValue(&ka, &cj, body, len, payload, attrName));
  }
  else
  {
    char* copy = corAlloc(&ka, len + 1);

    if (copy != NULL)
    {
      memcpy(copy, body, len);
      copy[len] = 0;
    }

    measureP = (copy != NULL) ? corJsonParse(&cj, copy) : NULL;
  }

  if ((measureP == NULL) || ((measureP->type != CorObject) && (measureP->type != CorArray)))
  {
    *nameP = "BAD_PAYLOAD";
    snprintf(message, messageSize, "a measure is a JSON object, or an array of them");
    status = 400;
  }
  else if (measureP->type == CorObject)
  {
    int64_t  time;
    CorNode* recordP = recordMake(&ka, measureP, apikey, deviceId, defaultTime, &time);

    if (recordP != NULL)
      recordSend(endpoint, recordP, time);
  }
  else
  {
    //
    // A multi-measure: one record per element, in ascending TimeInstant order, as iotagent-json writes them
    //
    int count = 0;

    for (CorNode* itemP = measureP->value.head; itemP != NULL; itemP = itemP->next)
      count++;

    Record* recordV = (count > 0) ? calloc(count, sizeof(Record)) : NULL;
    int     records = 0;

    for (CorNode* itemP = measureP->value.head; (itemP != NULL) && (recordV != NULL); itemP = itemP->next)
    {
      if (itemP->type != CorObject)
        continue;

      recordV[records].nodeP = recordMake(&ka, itemP, apikey, deviceId, defaultTime, &recordV[records].time);
      recordV[records].ix    = records;

      if (recordV[records].nodeP != NULL)
        records++;
    }

    if (records > 1)
      qsort(recordV, records, sizeof(Record), recordCompare);

    for (int ix = 0; ix < records; ix++)
      recordSend(endpoint, recordV[ix].nodeP, recordV[ix].time);

    free(recordV);
  }

  corAllocBufferReset(&ka, false);
  free(endpoint);
  return status;
}
