#ifndef IOTAJSON_EXECUTOR_H_
#define IOTAJSON_EXECUTOR_H_

//
// FILE            iotajsonExecutor.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The IoT Agent JSON's commands, as Service Executions - coraine doc/iot-agent-executor.md.
//
// A Service Registration whose endpoint is one of
//
//   iota-json+mqtt://<apikey>/<deviceId>/<cmd>[?qos=0|1|2][&retain=true]
//   iota-json+http://<host>[:<port>]/<path>?cmd=<cmd>       (iota-json+https too)
//   iota-json+poll://<apikey>/<deviceId>/<cmd>
//
// is executed here: the device gets {"<cmd>": <input.value>} - what iotagent-json sends it - on MQTT
// /<apikey>/<deviceId>/cmd, as the body of a POST to its endpoint, or in the answer to its own
// GET /iot/json?k=<apikey>&i=<deviceId>&getCmd=1 (poll). Its answer completes the execution:
// {"<cmd>": <result>} on /<apikey>/<deviceId>/cmdexe, the POST's 200/201 response, or its
// POST /iot/json/commands?k=<apikey>&i=<deviceId>.
//
#include "corTree/CorNode.h"                          // CorNode
#include "corBridge/BridgeBroker.h"                   // BridgeBroker



// -----------------------------------------------------------------------------
//
// iotajsonExecutorOpen - the MQTT connection ("server": { "uri": "mqtt://host[:port]" }, optional), the
// southbound port ("southbound": { "port": 7896 }, optional) and the HTTP worker; BRIDGE_OK or BRIDGE_ERR
//
extern int iotajsonExecutorOpen(CorNode* serverP, CorNode* southP, const BridgeBroker* brokerP);



// -----------------------------------------------------------------------------
//
// iotajsonExecutorClose - no thread of the plugin calls the broker after it
//
extern void iotajsonExecutorClose(void);



// -----------------------------------------------------------------------------
//
// iotajsonServiceExecute / iotajsonServiceCancel - BridgeDriver.serviceExecute / serviceCancel
//
extern int iotajsonServiceExecute(const char* url, const char* executionId, const char* inputJson);
extern int iotajsonServiceCancel(const char* url, const char* executionId);



// -----------------------------------------------------------------------------
//
// iotajsonPullDeliver - a device's getCmd=1: its pull commands not yet delivered, {"<cmd>": <value>, ...},
// malloc'd ("{}": none; NULL: out of memory) - delivered from here on
//
extern char* iotajsonPullDeliver(const char* apikey, const char* deviceId);



// -----------------------------------------------------------------------------
//
// iotajsonCommandResult - a device's POST /iot/json/commands: {"<cmd>": <result>, ...}, each completing the
// oldest execution of that command on that device
//
extern void iotajsonCommandResult(const char* apikey, const char* deviceId, const char* body, int len);



// -----------------------------------------------------------------------------
//
// iotajsonExecutorVersion - "libmosquitto x.y.z"
//
extern const char* iotajsonExecutorVersion(void);

#endif  // IOTAJSON_EXECUTOR_H_
