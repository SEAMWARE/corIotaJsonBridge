#ifndef IOTAJSON_SOUTH_H_
#define IOTAJSON_SOUTH_H_

//
// FILE            iotajsonSouth.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The southbound port - where a device is the client, as iotagent-json's (7896): served by the broker for
// the plugin (BridgeBroker.httpListen, bridge ABI 16), plain HTTP, no NGSI-LD and no authentication (a
// device names itself with k=<apikey>&i=<deviceId>, as it does to iotagent-json).
//
//   GET  /iot/json?k=<apikey>&i=<deviceId>&getCmd=1   the device's pull commands, {"<cmd>": <value>, ...}
//   POST /iot/json/commands?k=<apikey>&i=<deviceId>   {"<cmd>": <result>, ...} - the commands' answers
//   POST /iot/json, GET /iot/json?d=, POST /iot/json/attrs/<name>   measures (iotajsonMeasures.h)
//
#include "corBridge/BridgeBroker.h"                   // BridgeBroker



// -----------------------------------------------------------------------------
//
// iotajsonSouthOpen - ask the broker to serve 'port'; BRIDGE_OK or BRIDGE_ERR. No close: the broker stops
// serving it before the plugin's close()
//
extern int iotajsonSouthOpen(unsigned short port, const BridgeBroker* brokerP);

#endif  // IOTAJSON_SOUTH_H_
