// Loads z2m/growbox.mjs with the real zigbee-herdsman-converters and checks that every floor exposes exactly the
// readings its endpoint has clusters for, that configure only touches those endpoints, and that reports convert.
//
//   npm install --no-save --ignore-scripts zigbee-herdsman-converters@26.125.0   (in the repository root)
//   node test/z2m/test_converter.mjs
import assert from "node:assert/strict";
import {prepareDefinition} from "zigbee-herdsman-converters";
import {Zcl} from "zigbee-herdsman";
import converter from "../../z2m/growbox.mjs";

const definition = prepareDefinition(converter);

const T = 0x0402, H = 0x0405, P = 0x0403, I = 0x0400, SOIL = 0xfc01;
const FLOOR_BASE = [0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0xfc00];
const NAMES = {0xfc00: "growboxCtrl", 0xfc01: "growboxSoil"};
const clusterName = (id) => NAMES[id] ?? Zcl.Utils.getCluster(id, undefined, {}).name;

// Stand-ins named like herdsman's classes: zigbee-herdsman-converters tells them apart by constructor name.
class Endpoint {}
class Device {}

function makeDevice(layout, calls) {
    const endpoints = Object.entries(layout).map(([id, clusters]) => {
        const ID = Number(id);
        const inputClusters = ID === 4 ? [0x0003, ...clusters] : [...FLOOR_BASE, ...clusters];
        return Object.assign(new Endpoint(), {
            ID,
            inputClusters,
            outputClusters: [],
            deviceIeeeAddress: "0x01",
            getInputClusters: () => inputClusters.map((c) => ({ID: c, name: clusterName(c)})),
            getOutputClusters: () => [],
            supportsInputCluster: (c) => inputClusters.some((x) => x === c || clusterName(x) === c),
            supportsOutputCluster: () => false,
            getClusterAttributeValue: () => undefined,
            saveClusterAttributeKeyValue: () => {},
            bind: async (cluster) => calls.push(`bind ${ID} ${cluster}`),
            configureReporting: async (cluster) => calls.push(`report ${ID} ${cluster}`),
            read: async () => {},
            write: async () => {},
            save: () => {},
        });
    });
    return Object.assign(new Device(), {
        ieeeAddr: "0x01",
        modelID: "GrowBox-C6",
        manufacturerName: "GrowBoxDIY",
        type: "Router",
        powerSource: "Mains (single phase)",
        endpoints,
        customClusters: {},
        meta: {},
        getEndpoint: (id) => endpoints.find((e) => e.ID === id),
        addCustomCluster: () => {},
        save: () => {},
    });
}

const properties = (device) =>
    definition
        .exposes(device, {})
        .flatMap((e) => (e.features ? e.features.map((f) => f.property) : [e.property]))
        .filter((p) => /^(temperature|humidity|pressure|illuminance|soil_moisture)_/.test(p))
        .sort();

// Default sensors: floor 1 DHT11 + soil, floor 2 AHT10 + soil, floor 3 AHT10 + BMP180 + TEMT6000, no room sensor.
assert.deepEqual(
    properties(makeDevice({1: [T, H, SOIL], 2: [T, H, SOIL], 3: [T, H, P, I]}, [])),
    [
        "humidity_floor1", "humidity_floor2", "humidity_floor3", "illuminance_floor3", "pressure_floor3",
        "soil_moisture_floor1", "soil_moisture_floor2", "temperature_floor1", "temperature_floor2",
        "temperature_floor3",
    ],
);

// Other sensors: floor 1 light only, floor 2 BME280 + soil, floor 3 nothing, room sensor.
const calls = [];
const custom = makeDevice({1: [I], 2: [T, H, P, SOIL], 3: [], 4: [T, H]}, calls);
assert.deepEqual(properties(custom), [
    "humidity_floor2", "humidity_room", "illuminance_floor1", "pressure_floor2", "soil_moisture_floor2",
    "temperature_floor2", "temperature_room",
]);
await definition.configure(custom, Object.assign(new Endpoint(), {ID: 1, deviceIeeeAddress: "0x00"}), definition);
const sensorReports = calls
    .filter((c) => c.startsWith("report ") && !/genOnOff|genLevelCtrl|growboxCtrl/.test(c))
    .sort();
assert.deepEqual(sensorReports, [
    "report 1 msIlluminanceMeasurement",
    "report 2 growboxSoil",
    "report 2 msPressureMeasurement",
    "report 2 msRelativeHumidity",
    "report 2 msTemperatureMeasurement",
    "report 4 msRelativeHumidity",
    "report 4 msTemperatureMeasurement",
]);

// The docs and Home Assistant see every possible entity.
assert.ok(definition.exposes({isDummyDevice: true}, {}).length > 50);

// Reports from the device.
function convert(cluster, data, endpointId) {
    const out = {};
    const meta = {device: custom, state: {}};
    for (const fz of definition.fromZigbee.filter((f) => f.cluster === cluster)) {
        const msg = {data, cluster, type: "attributeReport", endpoint: {ID: endpointId}, device: custom};
        Object.assign(out, fz.convert(definition, msg, () => {}, {}, meta) ?? {});
    }
    return out;
}
assert.deepEqual(convert("growboxSoil", {soilMoisture: 5123}, 2), {soil_moisture_floor2: 51.2});
assert.deepEqual(convert("growboxSoil", {soilMoisture: 0xffff}, 2), {});
assert.deepEqual(convert("msTemperatureMeasurement", {measuredValue: 2345}, 4), {temperature_room: 23.45});
assert.deepEqual(convert("msTemperatureMeasurement", {measuredValue: -32768}, 1), {});
assert.deepEqual(convert("msIlluminanceMeasurement", {measuredValue: 30001}, 1), {illuminance_floor1: 1000});

console.log("converter checks passed");
