// Zigbee2MQTT external converter for the ESP32-C6 grow box (firmware in this repository).
// Install: copy to <z2m data>/external_converters/, set `advanced: enable_external_js: true`, restart Z2M.
//
// Endpoint N = floor N (1 = bottom). Per-floor properties get a `_floorN` suffix, e.g. `state_floor2`,
// `temperature_floor3`, `enabled_floor1`. Which sensors a floor has is set in the firmware's menuconfig; only the
// readings a floor really has are exposed (decided from the endpoint's cluster list, so after changing the sensors
// and flashing, click Interview and then Reconfigure on the device page). Optional room sensor next to the
// controller on endpoint 4 ("room": temperature_room, humidity_room, pressure_room for a BME280).
import {Zcl} from "zigbee-herdsman";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";
import {isDummyDevice} from "zigbee-herdsman-converters/lib/utils";

// Custom cluster 0xFC00 on every floor endpoint; attribute IDs must match main/zb_device.c.
const CL = "growboxCtrl";
// Soil moisture: custom cluster 0xFC01, only on floors with a soil sensor.
const SOIL_CL = "growboxSoil";
const ENDPOINTS = {floor1: 1, floor2: 2, floor3: 3, room: 4};
const FLOORS = ["floor1", "floor2", "floor3"];
const CLIMATE = [...FLOORS, "room"];
const CLUSTER_ID = {temperature: 0x0402, humidity: 0x0405, pressure: 0x0403, illuminance: 0x0400, soil: 0xfc01};

// zigbee-herdsman decodes ZCL BOOLEAN as the number 0/1, not true/false.
const ON_OFF = {valueOn: ["ON", 1], valueOff: ["OFF", 0]};
const SETTING_REPORTING = {min: 0, max: "1_HOUR", change: 1};
const FLAG_REPORTING = {min: 0, max: "1_HOUR", change: 0};

// Until a sensor has a reading (and on disabled floors) the firmware holds the ZCL "invalid" value
// (0x8000 for signed, 0xFFFF for unsigned attributes). Drop those instead of publishing -327.68 °C or 655.35 %.
const INVALID_S16 = -32768;
const INVALID_U16 = 0xffff;
function skipInvalid(extend, attribute, invalid) {
    for (const fz of extend.fromZigbee) {
        const convert = fz.convert;
        fz.convert = (model, msg, publish, options, meta) => {
            if (msg.data?.[attribute] === invalid) {
                return;
            }
            return convert(model, msg, publish, options, meta);
        };
    }
    return extend;
}

// Exposes and configures a per-endpoint reading only on the endpoints that have its cluster.
function whereSupported(extend, clusterId) {
    const has = (device, endpointName) =>
        device.endpoints.find((e) => e.ID === ENDPOINTS[endpointName])?.inputClusters.includes(clusterId) ?? false;
    extend.exposes = extend.exposes.map((expose) =>
        typeof expose === "function"
            ? expose
            : (device) => (isDummyDevice(device) || !expose.endpoint || has(device, expose.endpoint) ? [expose] : []),
    );
    // The configure steps loop over device.endpoints; hand them only the endpoints with the cluster.
    extend.configure = (extend.configure ?? []).map((configure) => (device, coordinatorEndpoint, definition) => {
        const filtered = new Proxy(device, {
            get(target, prop) {
                if (prop === "endpoints") {
                    return target.endpoints.filter((e) => e.inputClusters.includes(clusterId));
                }
                const value = Reflect.get(target, prop, target);
                return typeof value === "function" && prop !== "constructor" ? value.bind(target) : value;
            },
        });
        return configure(filtered, coordinatorEndpoint, definition);
    });
    return extend;
}

export default {
    zigbeeModel: ["GrowBox-C6"],
    model: "GrowBox-C6",
    vendor: "GrowBoxDIY",
    description: "ESP32-C6 3-floor grow box controller",
    extend: [
        m.deviceEndpoints({endpoints: ENDPOINTS}),
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
                floorEnabled: {name: "floorEnabled", ID: 0x0007, type: Zcl.DataType.BOOLEAN, write: true},
                timeSynced: {name: "timeSynced", ID: 0x0008, type: Zcl.DataType.BOOLEAN},
                lightOnMinute: {name: "lightOnMinute", ID: 0x0009, type: Zcl.DataType.UINT8, write: true, max: 59},
                lightOffMinute: {name: "lightOffMinute", ID: 0x000a, type: Zcl.DataType.UINT8, write: true, max: 59},
                sensorFault: {name: "sensorFault", ID: 0x000b, type: Zcl.DataType.BOOLEAN},
                takePhoto: {name: "takePhoto", ID: 0x000c, type: Zcl.DataType.BOOLEAN, write: true},
            },
            commands: {},
            commandsResponse: {},
        }),
        m.deviceAddCustomCluster(SOIL_CL, {
            name: SOIL_CL,
            ID: 0xfc01,
            attributes: {
                soilMoisture: {name: "soilMoisture", ID: 0x0000, type: Zcl.DataType.UINT16},
            },
            commands: {},
            commandsResponse: {},
        }),
        m.light({endpointNames: FLOORS, configureReporting: true, effect: false, powerOnBehavior: false}),
        whereSupported(
            skipInvalid(m.temperature({endpointNames: CLIMATE}), "measuredValue", INVALID_S16),
            CLUSTER_ID.temperature,
        ),
        whereSupported(skipInvalid(m.humidity({endpointNames: CLIMATE}), "measuredValue", INVALID_U16), CLUSTER_ID.humidity),
        // Firmware value unit is hPa, so report every 2 hPa.
        whereSupported(
            skipInvalid(
                m.pressure({endpointNames: CLIMATE, reporting: {min: "10_SECONDS", max: "1_HOUR", change: 2}}),
                "measuredValue",
                INVALID_S16,
            ),
            CLUSTER_ID.pressure,
        ),
        whereSupported(
            skipInvalid(m.illuminance({endpointNames: FLOORS}), "measuredValue", INVALID_U16),
            CLUSTER_ID.illuminance,
        ),
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
            name: "light_on_minute",
            cluster: CL,
            attribute: "lightOnMinute",
            endpointNames: FLOORS,
            valueMin: 0,
            valueMax: 59,
            valueStep: 1,
            unit: "min",
            access: "ALL",
            reporting: SETTING_REPORTING,
            description: "Minute the light turns on (auto mode)",
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
            name: "light_off_minute",
            cluster: CL,
            attribute: "lightOffMinute",
            endpointNames: FLOORS,
            valueMin: 0,
            valueMax: 59,
            valueStep: 1,
            unit: "min",
            access: "ALL",
            reporting: SETTING_REPORTING,
            description: "Minute the light turns off (auto mode)",
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
            description: "Over-temperature cutoff active (also when no temperature sensor works for a while)",
        }),
        m.binary({
            name: "sensor_fault",
            cluster: CL,
            attribute: "sensorFault",
            ...ON_OFF,
            endpointName: FLOORS,
            access: "STATE_GET",
            reporting: FLAG_REPORTING,
            description: "A sensor of this floor gave no reading at the last measurement",
        }),
        whereSupported(
            skipInvalid(
                m.numeric({
                    name: "soil_moisture",
                    cluster: SOIL_CL,
                    attribute: "soilMoisture",
                    endpointNames: FLOORS,
                    unit: "%",
                    scale: 100,
                    precision: 1,
                    access: "STATE_GET",
                    reporting: {min: 10, max: "1_HOUR", change: 100},
                    description: "Soil moisture (0 % = the dry calibration value, 100 % = in water)",
                }),
                "soilMoisture",
                INVALID_U16,
            ),
            CLUSTER_ID.soil,
        ),
        m.binary({
            name: "take_photo",
            cluster: CL,
            attribute: "takePhoto",
            ...ON_OFF,
            endpointName: FLOORS,
            access: "STATE_SET",
            description: "Only with cameras: set ON to take and upload a photo of this floor now (goes back to OFF)",
        }),
        m.binary({
            name: "time_synced",
            cluster: CL,
            attribute: "timeSynced",
            ...ON_OFF,
            access: "STATE_GET",
            entityCategory: "diagnostic",
            reporting: FLAG_REPORTING,
            description: "Clock set from the coordinator (kept across restarts, lost on power loss)",
        }),
    ],
    // Firmware updates: the build writes build/growbox.ota and build/growbox_ota_index.json (see README).
    ota: true,
};
