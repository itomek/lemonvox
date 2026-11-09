// Run under dbus-run-session. This exercises the real GIO proxy, name watcher,
// PropertiesChanged delivery, backend loss/reconnect and teardown without
// installing a GNOME Shell extension or touching user preferences.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {BackendClient, BUS_NAME, OBJECT_PATH} from '../extension/dbus.js';

const xmlFile = Gio.File.new_for_uri(import.meta.url)
    .get_parent()
    .get_parent()
    .get_child('extension')
    .get_child('dbus-interface.xml');
const [, bytes] = xmlFile.load_contents(null);
const interfaceXml = new TextDecoder().decode(bytes);

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

class FakeBackend {
    constructor() {
        this.calls = [];
        this._state = 'idle';
        this._message = 'Ready';
        this._result = '';
        this._palette = '#101112ff #202122ff #303132ff #404142ff';
        this._error = false;
        this.object = Gio.DBusExportedObject.wrapJSObject(interfaceXml, this);
    }

    get State() { return this._state; }
    get Message() { return this._message; }
    get Result() { return this._result; }
    get Palette() { return this._palette; }
    get Error() { return this._error; }

    Toggle() {
        this.calls.push('Toggle');
        this._set('State', 'recording');
        this._set('Message', 'Recording');
    }

    Stop() {
        this.calls.push('Stop');
        this._set('State', 'transcribing');
    }

    Cancel() {
        this.calls.push('Cancel');
        this._set('State', 'idle');
        this._set('Message', 'Ready');
        if (this._error) {
            this._set('Error', false);
        } else {
            this._set('Result', 'Synthetic result');
        }
    }

    ShowSettings() { this.calls.push('ShowSettings'); }

    ClearResult() {
        this.calls.push('ClearResult');
        this._set('Result', '');
    }

    Quit() { this.calls.push('Quit'); }

    export(connection) { this.object.export(connection, OBJECT_PATH); }
    destroy() { this.object.unexport(); }

    _set(property, value) {
        this[`_${property.toLowerCase()}`] = value;
        this.object.emit_property_changed(property, new GLib.Variant(
            typeof value === 'boolean' ? 'b' : 's', value));
    }
}

const loop = GLib.MainLoop.new(null, false);
let backend = null;
let ownerId = 0;
let client = null;
let stage = 'starting';
let callbackCount = 0;
let failure = null;

function startBackend() {
    backend = new FakeBackend();
    ownerId = Gio.bus_own_name(
        Gio.BusType.SESSION,
        BUS_NAME,
        Gio.BusNameOwnerFlags.NONE,
        connection => backend.export(connection),
        null,
        () => fail('failed to own fake LemonVox bus name'));
}

function stopBackend() {
    backend?.destroy();
    backend = null;
    if (ownerId) {
        Gio.bus_unown_name(ownerId);
        ownerId = 0;
    }
}

function fail(message) {
    if (!failure)
        failure = new Error(message);
    loop.quit();
}

function finish() {
    client?.destroy();
    stopBackend();
    loop.quit();
}

client = new BackendClient(snapshot => {
    callbackCount++;
    try {
        if (stage === 'starting') {
            assert(!snapshot.available, 'client must start unavailable');
            stage = 'awaiting-first-service';
            startBackend();
        } else if (stage === 'awaiting-first-service' && snapshot.available && snapshot.state === 'idle') {
            assert(snapshot.palette === '#101112ff #202122ff #303132ff #404142ff', 'client must receive Palette');
            assert(snapshot.error === false, 'client must receive false Error state');
            stage = 'awaiting-recording';
            client.request('Toggle');
        } else if (stage === 'awaiting-recording' && snapshot.state === 'recording') {
            assert(backend.calls.includes('Toggle'), 'Toggle must reach backend');
            stage = 'awaiting-result';
            client.request('Cancel');
        } else if (stage === 'awaiting-result' && snapshot.state === 'idle' && snapshot.result === 'Synthetic result') {
            assert(backend.calls.includes('Cancel'), 'Cancel must reach backend');
            stage = 'awaiting-clear';
            client.clearResult();
        } else if (stage === 'awaiting-clear' && snapshot.state === 'idle' && snapshot.result === '') {
            // clearResult intentionally clears the Shell's private copy before
            // its asynchronous D-Bus method has reached the backend.
            if (!backend.calls.includes('ClearResult'))
                return;
            assert(backend.calls.includes('ClearResult'), 'ClearResult must follow explicit copy');
            stage = 'awaiting-error';
            backend._set('Error', true);
            backend._set('Message', 'Synthetic error');
        } else if (stage === 'awaiting-error' && snapshot.error) {
            assert(snapshot.message === 'Synthetic error', 'client must receive error message');
            stage = 'awaiting-dismiss';
            client.request('Cancel');
        } else if (stage === 'awaiting-dismiss' && !snapshot.error) {
            assert(backend.calls.filter(call => call === 'Cancel').length === 2,
                'Cancel must dismiss a backend error');
            stage = 'awaiting-loss';
            stopBackend();
        } else if (stage === 'awaiting-loss' && !snapshot.available) {
            stage = 'awaiting-reconnect';
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, 20, () => {
                startBackend();
                return GLib.SOURCE_REMOVE;
            });
        } else if (stage === 'awaiting-reconnect' && snapshot.available && snapshot.state === 'idle') {
            const beforeDestroy = callbackCount;
            stage = 'destroyed';
            client.destroy();
            backend.Toggle();
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, 40, () => {
                assert(callbackCount === beforeDestroy, 'destroyed client must receive no late backend callback');
                finish();
                return GLib.SOURCE_REMOVE;
            });
        }
    } catch (error) {
        fail(error.message);
    }
});

GLib.timeout_add(GLib.PRIORITY_DEFAULT, 5000, () => {
    fail(`timed out while ${stage}`);
    return GLib.SOURCE_REMOVE;
});
client.start();
loop.run();

if (failure)
    throw failure;
print('PASS: D-Bus backend lifecycle, explicit result clearing and teardown');
