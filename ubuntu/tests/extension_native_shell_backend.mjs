// Synthetic in-session LemonVox service used only by the isolated Shell test.
// It owns the production D-Bus contract, but never captures audio or starts
// the application binary.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const BUS_NAME = 'com.itomek.LemonVox';
const OBJECT_PATH = '/com/itomek/LemonVox';
const readyFile = ARGV[0];
// Exercise the boundary between readable backend state and asynchronous proxy
// notification. This affects only this synthetic service, never the app.
const signalDelayMs = Number(GLib.getenv('LEMONVOX_TEST_SIGNAL_DELAY_MS') ?? '0');
if (!Number.isInteger(signalDelayMs) || signalDelayMs < 0 || signalDelayMs > 2000)
    throw new Error('invalid synthetic signal delay');
if (!readyFile)
    throw new Error('synthetic backend ready-file argument is required');

const xmlFile = Gio.File.new_for_uri(import.meta.url)
    .get_parent()
    .get_parent()
    .get_child('extension')
    .get_child('dbus-interface.xml');
const [, bytes] = xmlFile.load_contents(null);
const interfaceXml = new TextDecoder().decode(bytes);

class SyntheticBackend {
    constructor() {
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
        if (this._state === 'recording') {
            this._set('State', 'transcribing');
            this._set('Message', 'Transcribing');
        } else {
            this._set('Error', false);
            this._set('State', 'recording');
            this._set('Message', 'Recording');
        }
    }

    Stop() {
        this._set('State', 'transcribing');
        this._set('Message', 'Transcribing');
    }

    Cancel() {
        this._set('State', 'idle');
        this._set('Message', 'Ready');
        this._set('Result', '');
        this._set('Error', false);
    }

    ShowSettings() {}

    ClearResult() {
        this._set('Result', '');
    }

    Quit() {}

    setSyntheticError() {
        this._set('State', 'idle');
        this._set('Message', 'Synthetic recoverable error');
        this._set('Error', true);
    }

    setSyntheticResult() {
        this._set('State', 'idle');
        this._set('Message', 'Synthetic result ready');
        this._set('Error', false);
        this._set('Result', 'Synthetic manual copy text');
    }

    export(connection) {
        this.object.export(connection, OBJECT_PATH);
    }

    _set(property, value) {
        this[`_${property.toLowerCase()}`] = value;
        const emit = () => {
            this.object.emit_property_changed(property, new GLib.Variant(
                typeof value === 'boolean' ? 'b' : 's', value));
            return GLib.SOURCE_REMOVE;
        };
        if (signalDelayMs)
            GLib.timeout_add(GLib.PRIORITY_DEFAULT, signalDelayMs, emit);
        else
            emit();
    }
}

const backend = new SyntheticBackend();
const loop = GLib.MainLoop.new(null, false);
const ownerId = Gio.bus_own_name(
    Gio.BusType.SESSION,
    BUS_NAME,
    Gio.BusNameOwnerFlags.NONE,
    connection => backend.export(connection),
    () => GLib.file_set_contents(readyFile, 'ready\n'),
    () => loop.quit());

GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, () => {
    Gio.bus_unown_name(ownerId);
    loop.quit();
    return GLib.SOURCE_REMOVE;
});
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 10, () => {
    backend.setSyntheticError();
    return GLib.SOURCE_CONTINUE;
});
GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 12, () => {
    backend.setSyntheticResult();
    return GLib.SOURCE_CONTINUE;
});

loop.run();
