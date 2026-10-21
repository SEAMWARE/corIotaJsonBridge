#ifndef IOTAJSON_MEASURES_H_
#define IOTAJSON_MEASURES_H_

//
// FILE            iotajsonMeasures.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The IoT Agent JSON's measures, as DataChannel records (coraine doc/iot-agent-measures.md): a device's
// measure becomes a record - its keys, plus @deviceId and @apikey - handed to the broker on the device's
// DataChannel (<apikey>/<deviceId>), else its group's (<apikey>). The broker's mapping makes the entity.
//
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t

#include "corBridge/BridgeBroker.h"                   // BridgeBroker



// -----------------------------------------------------------------------------
//
// IotaPayload - what a single measure's body is (.../attrs/<name>): JSON, text, or bytes
//
typedef enum IotaPayload
{
  IotaPayloadJson   = 0,                              // JSON - anything else is taken as text (HTTP) or bytes (MQTT)
  IotaPayloadText   = 1,                              // text/plain: the value is the text
  IotaPayloadBinary = 2                               // application/octet-stream, or MQTT's non-JSON: the value is hex
} IotaPayload;



// -----------------------------------------------------------------------------
//
// iotajsonMeasuresOpen / Close -
//
extern void iotajsonMeasuresOpen(const BridgeBroker* brokerP);
extern void iotajsonMeasuresClose(void);



// -----------------------------------------------------------------------------
//
// iotajsonMeasureEndpointAdd / Del - a DataChannel's endpoint: <apikey> (a group) or <apikey>/<deviceId>
//
extern int iotajsonMeasureEndpointAdd(const char* endpoint);
extern int iotajsonMeasureEndpointDel(const char* endpoint);



// -----------------------------------------------------------------------------
//
// iotajsonMeasure - one measure from a device, on any transport
//
// @param body       the measure: a JSON object, an array of them (a multi-measure) - or, with attrName, the
//                   value of the one attribute
// @param attrName   NULL, or the attribute of a single measure (.../attrs/<name>)
// @param payload    with attrName: what the body is
// @param timeParam  the HTTP 't' parameter (ISO 8601), NULL when none
// @param nameP      OUT, on anything but 200: iotagent-json's error name
// @param messageP   OUT, on anything but 200: the message
//
// @return 200, 400 (the body cannot be used), 404 (no DataChannel - no group: DEVICE_GROUP_NOT_FOUND, a
//         group's devices but not this one: DEVICE_NOT_FOUND)
//
extern int iotajsonMeasure
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
);

#endif  // IOTAJSON_MEASURES_H_
