# corIotaJsonBridge

**Prototype - not released.** The FIWARE IoT Agent for JSON's **commands**, as **Service Executions**
in the coraine NGSI-LD broker: `iotajson.so`, a bridge plugin loaded with `--bridges iotajson`. The
design is coraine's `doc/iot-agent-executor.md`.

A device command is a Service Registration whose endpoint says how to reach the device; invoking the
service sends the device what iotagent-json sends it, and the device's answer completes the execution.

| Endpoint | The device gets | Its answer |
|---|---|---|
| `iota-json+mqtt://<apikey>/<deviceId>/<cmd>[?qos=0\|1\|2][&retain=true]` | `{"<cmd>": <value>}` on `/<apikey>/<deviceId>/cmd` | `{"<cmd>": <result>}` on `[/][json/]<apikey>/<deviceId>/cmdexe` |
| `iota-json+http[s]://<host>[:<port>]/<path>?cmd=<cmd>` | `POST` of `{"<cmd>": <value>}` to `http[s]://<host>/<path>` | the response, 200 or 201: `{"<cmd>": <result>}` |
| `iota-json+poll://<apikey>/<deviceId>/<cmd>` | nothing until it asks, on the southbound port: `GET /iot/json?k=<apikey>&i=<deviceId>&getCmd=1` answers `{"<cmd>": <value>, ...}` | `POST /iot/json/commands?k=<apikey>&i=<deviceId>`: `{"<cmd>": <result>}` |

- `<value>` is the execution input's `value` - the whole input without `id` and `type` when it has none.
- The execution ends `completed`, its `executionOutput` the device's `<result>`; `failed` when the
  device answers anything but 200/201 (HTTP), or not at all within the registration's
  `executionTimeout`.
- A pull command the device has asked for is still `executing`, with `executionProgress`
  `{"delivered": true}`. All the device's commands are handed over in one object, as iotagent-json hands
  them; a command stored twice, its later value.
- A pull command the device has not asked for yet can be cancelled; a command handed to a device cannot (409).

```json
{ "type": "ServiceRegistration",
  "entities": [ { "id": "urn:ngsi-ld:Lamp:lamp1", "type": "Lamp" } ],
  "serviceInformation": { "serviceName": "switch", "mode": "asynchronous" },
  "executionTimeout": "PT5M",
  "endpoint": "iota-json+mqtt://lamp-key/lamp1/switch" }
```

```
POST /ngsi-ld/v1/entities/urn:ngsi-ld:Lamp:lamp1/services/switch   { "value": "on" }
```

## Configuration

In the `--bridgeConfig` file: the MQTT server, and the southbound port - where a device is the client, as
iotagent-json's 7896. Without the server no MQTT commands, without the port no pull commands:

```json
{ "iotajson": { "server": { "uri": "mqtt://mosquitto:1883" }, "southbound": { "port": 7896 } } }
```

The southbound port is a libmicrohttpd daemon of the plugin's own, plain HTTP on every address, with no
NGSI-LD and no authentication: a device names itself with `k` and `i`, as it does to iotagent-json. Its
errors are iotagent-json's, `{"name": ..., "message": ...}`; a measure is a 501.

## Not built

Measures (a Channel is refused; on the southbound port a 501), the drop-in provisioning API
(`/iot/groups`, `/iot/devices`), lazy attributes, JEXL expressions, MQTT TLS and credentials.

## Build and test

    make && make test          # libmosquitto-dev, libmicrohttpd; the tests run mosquitto and python3-paho-mqtt

`test/funcTests/device.py` plays the devices, over a private mosquitto on port 11884.
