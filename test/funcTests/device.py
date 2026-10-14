#!/usr/bin/env python3
#
# FILE            device.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# device.py <mqttPort> <httpPort> <logFile> - a device as iotagent-json talks to it:
#
#   MQTT  a command on /<apikey>/<deviceId>/cmd, {"<cmd>": <value>} - answered on /<apikey>/<deviceId>/cmdexe
#         with {"<cmd>": "<value> done"}; a device of apikey 'silent' never answers
#   HTTP  a command POSTed to the device's endpoint - answered 200 {"<cmd>": "<value> done"}; a path under
#         /fail answers 500
#
# Every command received is one line of the log: MQTT <topic> <payload> / HTTP <path> <body>.
#
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import paho.mqtt.client as mqtt

mqttPort = int(sys.argv[1])
httpPort = int(sys.argv[2])
logFile  = sys.argv[3]
lock     = threading.Lock()


def log(line):
    with lock:
        with open(logFile, "a") as f:
            f.write(line + "\n")


def answer(body):
    cmd = json.loads(body)
    return {name: "%s done" % (value if isinstance(value, str) else json.dumps(value)) for name, value in cmd.items()}


def onConnect(client, userdata, flags, rc, properties=None):
    client.subscribe("/+/+/cmd", qos=1)
    log("READY")


def onMessage(client, userdata, msg):
    payload = msg.payload.decode()
    log("MQTT %s %s" % (msg.topic, payload))
    level = msg.topic.split("/")             # '', apikey, deviceId, 'cmd'
    if level[1] != "silent":
        client.publish("/%s/%s/cmdexe" % (level[1], level[2]), json.dumps(answer(payload)), qos=1)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        log("HTTP %s %s" % (self.path, body))
        if self.path.startswith("/fail"):
            self.send_response(500)
            self.end_headers()
            return
        out = json.dumps(answer(body)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    def log_message(self, *args):
        pass


client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = onConnect
client.on_message = onMessage
client.connect("localhost", mqttPort)
client.loop_start()

HTTPServer(("localhost", httpPort), Handler).serve_forever()
