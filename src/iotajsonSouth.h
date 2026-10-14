#ifndef IOTAJSON_SOUTH_H_
#define IOTAJSON_SOUTH_H_

//
// FILE            iotajsonSouth.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// The southbound port - where a device is the client, as iotagent-json's (7896): a libmicrohttpd daemon of
// the plugin's own, one thread, plain HTTP, no NGSI-LD and no authentication (a device names itself with
// k=<apikey>&i=<deviceId>, as it does to iotagent-json).
//
//   GET  /iot/json?k=<apikey>&i=<deviceId>&getCmd=1   the device's pull commands, {"<cmd>": <value>, ...}
//   POST /iot/json/commands?k=<apikey>&i=<deviceId>   {"<cmd>": <result>, ...} - the commands' answers
//
// Measures (POST /iot/json, GET /iot/json?d=...) are not built: 501.
//



// -----------------------------------------------------------------------------
//
// iotajsonSouthOpen - listen on 'port', all addresses; BRIDGE_OK or BRIDGE_ERR
//
extern int iotajsonSouthOpen(unsigned short port);



// -----------------------------------------------------------------------------
//
// iotajsonSouthClose - no request in flight after it
//
extern void iotajsonSouthClose(void);

#endif  // IOTAJSON_SOUTH_H_
