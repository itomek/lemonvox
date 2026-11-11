import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {normalizeBackendState} from './policy.js';

export const BUS_NAME = 'com.itomek.LemonVox';
export const OBJECT_PATH = '/com/itomek/LemonVox';
export const INTERFACE_NAME = 'com.itomek.LemonVox';

const CONTROL_METHODS = new Set([
    'Toggle',
    'Stop',
    'Cancel',
    'ShowSettings',
    'ClearResult',
    'Quit',
]);

function safeMessage(value) {
    if (typeof value !== 'string')
        return '';
    return value.slice(0, 240);
}

function propertyString(proxy, name) {
    const property = proxy.get_cached_property(name);
    return property ? property.deep_unpack() : '';
}

function propertyBoolean(proxy, name) {
    const property = proxy.get_cached_property(name);
    return property ? property.deep_unpack() === true : false;
}

// The Shell holds Result until delivery, then asks the application to discard it.
export class BackendClient {
    constructor(onChanged) {
        this._onChanged = onChanged;
        this._destroyed = false;
        this._generation = 0;
        this._watcherId = 0;
        this._propertySignal = 0;
        this._ownerSignal = 0;
        this._proxy = null;
        this._connecting = false;
        this._pendingMethod = null;
        this._snapshot = {
            available: false,
            state: 'unavailable',
            message: 'LemonVox is unavailable.',
            result: '',
            palette: '',
            error: false,
        };
    }

    get snapshot() {
        return {...this._snapshot};
    }

    start() {
        if (this._destroyed || this._watcherId)
            return;
        this._watcherId = Gio.DBus.session.watch_name(
            BUS_NAME,
            Gio.BusNameWatcherFlags.NONE,
            () => this._connect(false),
            () => this._markUnavailable());
    }

    request(method) {
        if (!CONTROL_METHODS.has(method))
            throw new Error(`Unsupported LemonVox control method: ${method}`);
        if (this._destroyed)
            return;
        if (this._proxy) {
            this._call(method);
            return;
        }

        // A second toggle while D-Bus activation is still pending must not turn
        // a just-started session straight back into a stop request.
        if (this._pendingMethod)
            return;
        this._pendingMethod = method;
        this._connect(true);
    }

    clearResult() {
        if (this._destroyed || !this._snapshot.result)
            return;
        this._snapshot.result = '';
        this._notify();
        this.request('ClearResult');
    }

    destroy() {
        this._destroyed = true;
        this._generation++;
        this._pendingMethod = null;
        if (this._watcherId) {
            Gio.DBus.session.unwatch_name(this._watcherId);
            this._watcherId = 0;
        }
        this._detachProxy();
        this._onChanged = null;
    }

    _connect(allowActivation) {
        if (this._destroyed || this._proxy || this._connecting)
            return;

        this._connecting = true;
        const generation = ++this._generation;
        const proxy = new Gio.DBusProxy({
            gConnection: Gio.DBus.session,
            gName: BUS_NAME,
            gObjectPath: OBJECT_PATH,
            gInterfaceName: INTERFACE_NAME,
            gFlags: allowActivation
                ? Gio.DBusProxyFlags.NONE
                : Gio.DBusProxyFlags.DO_NOT_AUTO_START,
        });
        proxy.init_async(GLib.PRIORITY_DEFAULT, null, (source, result) => {
            try {
                source.init_finish(result);
            } catch (_error) {
                if (!this._destroyed && generation === this._generation) {
                    this._connecting = false;
                    this._pendingMethod = null;
                    this._markUnavailable();
                }
                return;
            }
            if (this._destroyed || generation !== this._generation)
                return;
            this._connecting = false;
            this._attachProxy(proxy);
            const pending = this._pendingMethod;
            this._pendingMethod = null;
            if (pending)
                this._call(pending);
        });
    }

    _attachProxy(proxy) {
        this._detachProxy();
        if (!proxy.gNameOwner) {
            this._markUnavailable();
            return;
        }
        this._proxy = proxy;
        this._propertySignal = this._proxy.connect('g-properties-changed', () => this._readProperties());
        this._ownerSignal = this._proxy.connect('notify::g-name-owner', () => {
            if (!this._proxy?.gNameOwner)
                this._markUnavailable();
        });
        this._readProperties();
    }

    _detachProxy() {
        if (this._proxy && this._propertySignal)
            this._proxy.disconnect(this._propertySignal);
        if (this._proxy && this._ownerSignal)
            this._proxy.disconnect(this._ownerSignal);
        this._propertySignal = 0;
        this._ownerSignal = 0;
        this._proxy = null;
    }

    _readProperties() {
        if (!this._proxy || this._destroyed)
            return;
        const state = normalizeBackendState(propertyString(this._proxy, 'State'));
        this._snapshot = {
            available: true,
            state,
            message: safeMessage(propertyString(this._proxy, 'Message')),
            result: propertyString(this._proxy, 'Result'),
            palette: propertyString(this._proxy, 'Palette'),
            error: propertyBoolean(this._proxy, 'Error'),
        };
        this._notify();
    }

    _call(method) {
        if (!this._proxy || this._destroyed)
            return;
        if (!this._proxy.gNameOwner) {
            this._markUnavailable();
            return;
        }
        const generation = this._generation;
        this._proxy.call(
            method,
            null,
            Gio.DBusCallFlags.NONE,
            -1,
            null,
            (proxy, result) => {
                try {
                    proxy.call_finish(result);
                } catch (_error) {
                    if (!this._destroyed && generation === this._generation)
                        this._markUnavailable();
                }
            });
    }

    _markUnavailable() {
        if (this._destroyed)
            return;
        // A queued proxy initialization may complete after the service vanishes.
        // Invalidate it and permit a later name-appeared event to reconnect.
        this._generation++;
        this._connecting = false;
        this._pendingMethod = null;
        this._detachProxy();
        this._snapshot = {
            available: false,
            state: 'unavailable',
            message: 'LemonVox is unavailable.',
            result: '',
            palette: '',
            error: false,
        };
        this._notify();
    }

    _notify() {
        this._onChanged?.(this.snapshot);
    }
}
