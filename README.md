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

- `<value>` is the execution input's `value` - the whole input without `id` and `type` when it has none.
- The execution ends `completed`, its `executionOutput` the device's `<result>`; `failed` when the
  device answers anything but 200/201 (HTTP), or not at all within the registration's
  `executionTimeout`.
- A command handed to a device cannot be cancelled (409).

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

The MQTT server, in the `--bridgeConfig` file; without it, HTTP commands only:

```json
{ "iotajson": { "server": { "uri": "mqtt://mosquitto:1883" } } }
```

## Not built

Measures (a Channel is refused), pull commands (`getCmd=1`), the drop-in provisioning API
(`/iot/groups`, `/iot/devices`), lazy attributes, JEXL expressions, MQTT TLS and credentials.

## Build and test

    make && make test          # libmosquitto-dev; the test runs mosquitto and python3-paho-mqtt

`test/funcTests/device.py` plays the devices, over a private mosquitto on port 11884.
