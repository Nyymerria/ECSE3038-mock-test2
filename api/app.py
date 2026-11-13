
import os
import json
import secrets
from datetime import datetime, timezone

from fastapi import FastAPI, HTTPException, Header
from pydantic import BaseModel, Field
from pymongo import MongoClient, DESCENDING
from dotenv import load_dotenv
import paho.mqtt.client as mqtt

# ----------------------------------
# CONFIGURATION
# ----------------------------------

load_dotenv()

MONGODB_URI = os.getenv("MONGODB_URI")
MQTT_NAME = os.getenv("MQTT_NAME")

if not MONGODB_URI or not MQTT_NAME:
    raise RuntimeError("Set MONGODB_URI and MQTT_NAME in .env")

MQTT_BROKER = "135.232.203.144"
MQTT_PORT = 1883

app = FastAPI()

# ----------------------------------
# DATABASE
# ----------------------------------

client = MongoClient(MONGODB_URI)

db = client["cold_room_db"]

devices = db["devices"]
readings = db["readings"]
alarms = db["alarms"]

# Ensure each MAC is registered once
devices.create_index("mac", unique=True)
alarms.create_index("mac", unique=True)

# ----------------------------------
# MQTT
# ----------------------------------

mqtt_client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2
)

mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()

# ----------------------------------
# MODELS
# ----------------------------------

class Device(BaseModel):
    mac: str
    room: str


class Reading(BaseModel):
    mac: str
    temp: float = Field(ge=-40, le=80)


class Alarm(BaseModel):
    max_temp: float


# ----------------------------------
# HOME
# ----------------------------------

@app.get("/")
def home():
    return {"message": "Cold Room API running"}


# ----------------------------------
# A1: REGISTER DEVICE
# ----------------------------------

@app.post("/devices", status_code=201)
def register_device(device: Device):

    if devices.find_one({"mac": device.mac}):
        raise HTTPException(
            status_code=409,
            detail="Device already exists"
        )

    api_key = secrets.token_urlsafe(16)

    data = {
        "mac": device.mac,
        "room": device.room,
        "api_key": api_key
    }

    try:
        devices.insert_one(data)
    except Exception as exc:
        from pymongo.errors import DuplicateKeyError
        if isinstance(exc, DuplicateKeyError):
            raise HTTPException(
                status_code=409,
                detail="Device already exists"
            )
        raise

    return {
        "mac": device.mac,
        "room": device.room,
        "api_key": api_key
    }


# ----------------------------------
# GET DEVICES (NO SECRET KEYS)
# ----------------------------------

@app.get("/devices")
def get_devices():

    result = list(
        devices.find(
            {},
            {"_id": 0, "api_key": 0}
        )
    )

    return result


# ----------------------------------
# A2: CREATE TEMPERATURE READING
# ----------------------------------

@app.post("/readings", status_code=201)
def create_reading(
    reading: Reading,
    x_api_key: str | None = Header(default=None)
):

    # Missing or unknown key = 401
    if not x_api_key:
        raise HTTPException(
            status_code=401,
            detail="Missing API key"
        )

    owner = devices.find_one({
        "api_key": x_api_key
    })

    if owner is None:
        raise HTTPException(
            status_code=401,
            detail="Invalid API key"
        )

    # Valid key, wrong MAC = 403
    if owner["mac"] != reading.mac:
        raise HTTPException(
            status_code=403,
            detail="Key belongs to another device"
        )

    data = {
        "mac": reading.mac,
        "temp": reading.temp,
        "time": datetime.now(timezone.utc)
    }

    readings.insert_one(data)

    return {
        "mac": reading.mac,
        "temp": reading.temp,
        "time": data["time"].isoformat()
    }


# ----------------------------------
# A3: GET TEMPERATURE READINGS
# ----------------------------------

@app.get("/readings")
def get_readings(
    mac: str | None = None,
    since: datetime | None = None
):

    query = {}

    if mac is not None:
        query["mac"] = mac

    if since is not None:
        if since.tzinfo is None:
            since = since.replace(tzinfo=timezone.utc)

        query["time"] = {
            "$gte": since.astimezone(timezone.utc)
        }

    result = []

    cursor = readings.find(
        query,
        {"_id": 0}
    ).sort("time", DESCENDING)

    for item in cursor:
        item["time"] = item["time"].isoformat()
        result.append(item)

    return result


# ----------------------------------
# A4: SET ALARM LIMIT
# ----------------------------------

@app.put("/alarm/{mac}")
def set_alarm(mac: str, alarm: Alarm):

    # Device must exist
    device = devices.find_one({"mac": mac})

    if device is None:
        raise HTTPException(
            status_code=404,
            detail="Device not found"
        )

    existing = alarms.find_one({"mac": mac})

    alarms.update_one(
        {"mac": mac},
        {
            "$set": {
                "mac": mac,
                "max_temp": alarm.max_temp
            }
        },
        upsert=True
    )

    # MQTT topic
    topic = f"ecse3038/{MQTT_NAME}/alarm/max"

    # Publish retained limit
    info = mqtt_client.publish(
        topic,
        str(alarm.max_temp),
        qos=1,
        retain=True
    )

    info.wait_for_publish(timeout=5)

    if not info.is_published():
        raise HTTPException(
            status_code=503,
            detail="Could not publish MQTT alarm limit"
        )

    if existing is None:
        from fastapi.responses import JSONResponse
        return JSONResponse(
            status_code=201,
            content={
                "mac": mac,
                "max_temp": alarm.max_temp
            }
        )

    return {
        "mac": mac,
        "max_temp": alarm.max_temp
    }
