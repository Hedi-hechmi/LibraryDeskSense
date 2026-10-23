"""
LibraryDeskSense Proxy
- Receives MQTT messages from ESP32 and stores them in InfluxDB.
- Runs an HTTP server (port 8080) for ESP32 to POST telemetry.
- Runs a CoAP server (port 5683) for ESP32 to POST telemetry.
(Version 4 - MQTT + HTTP + CoAP + InfluxDB)
"""

import json
import threading
from datetime import datetime
import paho.mqtt.client as mqtt
from flask import Flask, request, jsonify
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
import asyncio
from aiocoap import Context, Message, Code
from aiocoap import resource

# Configuration 
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_TOPICS = "desk/#"

COAP_PORT = 5683
HTTP_PORT = 8080

# InfluxDB
INFLUX_URL = "http://localhost:8086"
INFLUX_TOKEN = "n3dZ8vwowLKX80Z8nGKHGGSMMxeFBa8rZeJ3YGev8yGD9FYbBhi5QTnF69clJafTryoGVpusOazurdP8Mh9R1g=="
INFLUX_ORG = "LibraryDeskSense"
INFLUX_BUCKET = "library_desk"


# InfluxDB Section
influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = influx_client.write_api(write_options=SYNCHRONOUS)


def write_telemetry_to_influx(source, payload_dict):
    """Write a telemetry data point to InfluxDB."""
    try:
        point = Point("telemetry").tag("source", source)

        if "dist" in payload_dict:
            point = point.field("dist", float(payload_dict["dist"]))
        if "noise" in payload_dict:
            point = point.field("noise", int(payload_dict["noise"]))
        if "light" in payload_dict:
            point = point.field("light", int(payload_dict["light"]))
        if "occupied" in payload_dict:
            point = point.field("occupied", bool(payload_dict["occupied"]))

        write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    except Exception as e:
        print(f"[InfluxDB] Write error (telemetry): {e}")


def write_event_to_influx(event_name, payload_str):
    """Write an event point to InfluxDB."""
    try:
        point = Point("event").tag("event_name", event_name).field("value", payload_str)
        write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    except Exception as e:
        print(f"[InfluxDB] Write error (event): {e}")


#  MQTT Section 
def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"[MQTT] Connected to broker at {MQTT_BROKER}:{MQTT_PORT}")
        client.subscribe(MQTT_TOPICS)
        print(f"[MQTT] Subscribed to topic: {MQTT_TOPICS}")
    else:
        print(f"[MQTT] Connection failed with code {rc}")


def on_message(client, userdata, msg):
    timestamp = datetime.now().strftime("%H:%M:%S")
    topic = msg.topic
    payload = msg.payload.decode("utf-8")
    print(f"[{timestamp}] MQTT  {topic} -> {payload}")

    # Telemetry topic? Parse JSON and write to InfluxDB
    if topic == "desk/telemetry":
        try:
            data = json.loads(payload)
            write_telemetry_to_influx("mqtt", data)
        except json.JSONDecodeError:
            print(f"[InfluxDB] Invalid JSON: {payload}")

    # Event topic? Write event
    elif topic.startswith("desk/events/"):
        event_name = topic.replace("desk/events/", "")
        write_event_to_influx(event_name, payload)


def start_mqtt():
    """Run the MQTT client (blocking)."""
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_forever()

# CoAP Section 
class TelemetryCoAPResource(resource.Resource):
    """CoAP resource that receives telemetry POSTs from ESP32."""
    async def render_post(self, request):
        timestamp = datetime.now().strftime("%H:%M:%S")
        payload = request.payload.decode("utf-8")
        print(f"[{timestamp}] CoAP  /telemetry -> {payload}")
        try:
            data = json.loads(payload)
            write_telemetry_to_influx("coap", data)
        except json.JSONDecodeError:
            print(f"[InfluxDB] Invalid CoAP JSON: {payload}")
        return Message(code=Code.CHANGED, payload=b"ok")


async def coap_server_main():
    root = resource.Site()
    root.add_resource(['telemetry'], TelemetryCoAPResource())
    await Context.create_server_context(root, bind=("0.0.0.0", COAP_PORT))
    print(f"[CoAP] Server listening on port {COAP_PORT}")
    await asyncio.get_event_loop().create_future()


def start_coap():
    asyncio.run(coap_server_main())
# HTTP Section 
app = Flask(__name__)


@app.route("/telemetry", methods=["POST"])
def receive_telemetry():
    timestamp = datetime.now().strftime("%H:%M:%S")
    try:
        data = request.get_json(force=True)
        print(f"[{timestamp}] HTTP  /telemetry -> {data}")
        write_telemetry_to_influx("http", data)
        return jsonify({"status": "ok"}), 200
    except Exception as e:
        print(f"[{timestamp}] HTTP error: {e}")
        return jsonify({"status": "error", "message": str(e)}), 400


def start_http():
    print(f"[HTTP] Server starting on port {HTTP_PORT}")
    app.run(host="0.0.0.0", port=HTTP_PORT, debug=False, use_reloader=False)


# Main
def main():
    print("=== LibraryDeskSense Proxy starting ===")
    print(f"[InfluxDB] Connecting to {INFLUX_URL}, org={INFLUX_ORG}, bucket={INFLUX_BUCKET}")

    mqtt_thread = threading.Thread(target=start_mqtt, daemon=True)
    mqtt_thread.start()
    coap_thread = threading.Thread(target=start_coap, daemon=True)
    coap_thread.start()

    start_http()


if __name__ == "__main__":
    main()