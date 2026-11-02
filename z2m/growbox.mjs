// Zigbee2MQTT external converter for the ESP32-C6 grow box (firmware in this repository).
// Install: copy to <z2m data>/external_converters/, set `advanced: enable_external_js: true`, restart Z2M.
//
// Endpoint N = floor N (1 = bottom). Per-floor properties get a `_floorN` suffix, e.g. `state_floor2`,
// `temperature_floor3`, `enabled_floor1`. Sensors: floor 1 DHT11 + capacitive soil, floor 2 AHT10 + YL-69 soil,
// floor 3 AHT10 + BMP180 (pressure) + TEMT6000 (illuminance).
import {Zcl} from "zigbee-herdsman";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";

// Custom cluster 0xFC00 on every floor endpoint; attribute IDs must match main/zb_device.c.
const CL = "growboxCtrl";
const FLOORS = ["floor1", "floor2", "floor3"];
const SOIL_FLOORS = ["floor1", "floor2"];

// zigbee-herdsman decodes ZCL BOOLEAN as the number 0/1, not true/false.
const ON_OFF = {valueOn: ["ON", 1], valueOff: ["OFF", 0]};
const SETTING_REPORTING = {min: 0, max: "1_HOUR", change: 1};
const FLAG_REPORTING = {min: 0, max: "1_HOUR", change: 0};

export default {
    zigbeeModel: ["GrowBox-C6"],
    model: "GrowBox-C6",
    vendor: "GrowBoxDIY",
    description: "ESP32-C6 3-floor grow box controller (router)",
    extend: [
        m.deviceEndpoints({endpoints: {floor1: 1, floor2: 2, floor3: 3}}),
        m.deviceAddCustomCluster(CL, {
            name: CL,
            ID: 0xfc00,
            attributes: {
                autoMode: {name: "autoMode", ID: 0x0000, type: Zcl.DataType.BOOLEAN, write: true},
                lightOnHour: {name: "lightOnHour", ID: 0x0001, type: Zcl.DataType.UINT8, write: true, max: 23},
                lightOffHour: {name: "lightOffHour", ID: 0x0002, type: Zcl.DataType.UINT8, write: true, max: 23},
                autoBrightness: {name: "autoBrightness", ID: 0x0003, type: Zcl.DataType.UINT8, write: true, min: 1, max: 100},
                maxTemp: {name: "maxTemp", ID: 0x0004, type: Zcl.DataType.UINT8, write: true, min: 20, max: 50},
                heatLock: {name: "heatLock", ID: 0x0005, type: Zcl.DataType.BOOLEAN},
                soilMoisture: {name: "soilMoisture", ID: 0x0006, type: Zcl.DataType.UINT16},
                floorEnabled: {name: "floorEnabled", ID: 0x0007, type: Zcl.DataType.BOOLEAN, write: true},
                timeSynced: {name: "timeSynced", ID: 0x0008, type: Zcl.DataType.BOOLEAN},
            },
            commands: {},
            commandsResponse: {},
        }),
        m.light({endpointNames: FLOORS, configureReporting: true, effect: false, powerOnBehavior: false}),
        m.temperature({endpointNames: FLOORS}),
        m.humidity({endpointNames: FLOORS}),
        // Library default reports only on a 5 kPa change; firmware value unit is hPa, so report every 2 hPa.
        m.pressure({endpointNames: ["floor3"], reporting: {min: "10_SECONDS", max: "1_HOUR", change: 2}}),
        m.illuminance({endpointNames: ["floor3"]}),
        m.binary({
            name: "enabled",
            cluster: CL,
            attribute: "floorEnabled",
            ...ON_OFF,
            endpointName: FLOORS, // m.binary takes `endpointName`, m.numeric takes `endpointNames`
            access: "ALL",
            reporting: FLAG_REPORTING,
            description: "Floor in use; OFF = its sensors are not read and its light stays off",
        }),
        m.binary({
            name: "auto_mode",
            cluster: CL,
            attribute: "autoMode",
            ...ON_OFF,
            endpointName: FLOORS,
            access: "ALL",
            reporting: FLAG_REPORTING,
            description: "Schedule controls the light; any manual light command switches this OFF",
        }),
        m.numeric({
            name: "light_on_hour",
            cluster: CL,
            attribute: "lightOnHour",
            endpointNames: FLOORS,
            valueMin: 0,
            valueMax: 23,
            valueStep: 1,
            unit: "h",
            access: "ALL",
            reporting: SETTING_REPORTING,
            description: "Hour the light turns on (auto mode)",
        }),
        m.numeric({
            name: "light_off_hour",
            cluster: CL,
            attribute: "lightOffHour",
            endpointNames: FLOORS,
            valueMin: 0,
            valueMax: 23,
            valueStep: 1,
            unit: "h",
            access: "ALL",
            reporting: SETTING_REPORTING,
            description: "Hour the light turns off (auto mode)",
        }),
        m.numeric({
            name: "auto_brightness",
            cluster: CL,
            attribute: "autoBrightness",
            endpointNames: FLOORS,
            valueMin: 1,
            valueMax: 100,
            valueStep: 1,
            unit: "%",
            access: "ALL",
            reporting: SETTING_REPORTING,
            description: "Brightness used in auto mode",
        }),
        m.numeric({
            name: "max_temperature",
            cluster: CL,
            attribute: "maxTemp",
            endpointNames: FLOORS,
            valueMin: 20,
            valueMax: 50,
            valueStep: 1,
            unit: "°C",
            access: "ALL",
            reporting: SETTING_REPORTING,
            description: "Light is forced off above this; resumes 2 °C below",
        }),
        m.binary({
            name: "heat_lock",
            cluster: CL,
            attribute: "heatLock",
            ...ON_OFF,
            endpointName: FLOORS,
            access: "STATE_GET",
            reporting: FLAG_REPORTING,
            description: "Over-temperature cutoff active",
        }),
        m.numeric({
            name: "soil_moisture",
            cluster: CL,
            attribute: "soilMoisture",
            endpointNames: SOIL_FLOORS,
            unit: "%",
            scale: 100,
            precision: 1,
            access: "STATE_GET",
            reporting: {min: 10, max: "1_HOUR", change: 100},
            description: "Soil moisture (floor 1 capacitive, floor 2 YL-69)",
        }),
        m.binary({
            name: "time_synced",
            cluster: CL,
            attribute: "timeSynced",
            ...ON_OFF,
            access: "STATE_GET",
            entityCategory: "diagnostic",
            reporting: FLAG_REPORTING,
            description: "Clock synced from coordinator",
        }),
    ],
};
