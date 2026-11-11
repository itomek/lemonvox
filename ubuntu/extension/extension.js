import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Extension from 'resource:///org/gnome/shell/extensions/extension.js';

import {BackendClient} from './dbus.js';
import {ShortcutBindings} from './keybindings.js';
import {activeState, normalizeBackendState, ResultDelivery} from './policy.js';
import {LemonVoxIndicator, SpeechBar} from './ui.js';

export default class LemonVoxExtension extends Extension.Extension {
    enable() {
        this._settings = this.getSettings();
        this._delivery = new ResultDelivery();
        this._pasteSource = 0;
        this._pasteGeneration = 0;
        this._keyboard = Clutter.get_default_backend().get_default_seat()
            .create_virtual_device(Clutter.InputDeviceType.KEYBOARD_DEVICE);
        this._shortcutProblem = '';
        this._client = new BackendClient(snapshot => this._update(snapshot));
        const actions = {
            toggle: () => this._client.request('Toggle'),
            stop: () => this._client.request('Stop'),
            cancel: () => this._dismissOrCancel(),
            clearResult: () => this._client.clearResult(),
            settings: () => this._client.request('ShowSettings'),
            quit: () => this._client.request('Quit'),
        };
        this._bar = new SpeechBar(actions);
        this._indicator = new LemonVoxIndicator(actions);
        this._bindings = new ShortcutBindings(
            this._settings,
            actions.toggle,
            actions.cancel,
            message => this._showShortcutProblem(message),
            () => this._clearShortcutProblem());
        this._client.start();
        this._update(this._client.snapshot);
    }

    disable() {
        this._cancelPaste();
        this._keyboard = null;
        this._delivery = null;
        // Best effort only: preserve the user's explicit cancel semantics before
        // removing the only visible controls. The backend independently drops
        // late work if the D-Bus call races teardown.
        if (this._client && activeState(this._client.snapshot.state))
            this._client.request('Cancel');
        this._bindings?.destroy();
        this._bindings = null;
        this._bar?.destroy();
        this._bar = null;
        this._indicator?.destroy();
        this._indicator = null;
        this._client?.destroy();
        this._client = null;
        this._settings = null;
        this._shortcutProblem = '';
    }

    _update(snapshot) {
        if (!this._indicator || !this._bar || !this._bindings)
            return;
        if (this._delivery.take(snapshot))
            this._schedulePaste(snapshot.result);
        if (!snapshot.available || activeState(snapshot.state) || snapshot.error)
            this._cancelPaste();
        let effective = this._effectiveSnapshot(snapshot);
        const state = normalizeBackendState(effective.state);
        this._bindings.setActive(activeState(state));
        // A failed registration can be reported synchronously by setActive().
        // Recompute after it returns so the outer update cannot hide its error
        // bar after the nested problem callback made it visible.
        effective = this._effectiveSnapshot(snapshot);
        this._bar.update({...effective, state});
        this._indicator.update({...effective, state});
    }

    _cancelPaste() {
        this._pasteGeneration++;
        if (this._pasteSource) {
            GLib.source_remove(this._pasteSource);
            this._pasteSource = 0;
        }
    }

    _schedulePaste(text) {
        this._cancelPaste();
        // The destination is whichever application has keyboard focus when
        // processing finishes. Field content and accessibility are not queried.
        if (!global.display.focus_window || !this._keyboard)
            return;
        const generation = this._pasteGeneration;
        const deadline = GLib.get_monotonic_time() + 5 * 1000 * 1000;
        this._pasteSource = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 25, () => {
            if (generation !== this._pasteGeneration)
                return GLib.SOURCE_REMOVE;
            if (!global.display.focus_window) {
                this._pasteSource = 0;
                return GLib.SOURCE_REMOVE;
            }
            const modifiers = global.get_pointer()[2] &
                (Clutter.ModifierType.SHIFT_MASK | Clutter.ModifierType.CONTROL_MASK |
                 Clutter.ModifierType.MOD1_MASK | Clutter.ModifierType.SUPER_MASK);
            if (modifiers) {
                if (GLib.get_monotonic_time() < deadline)
                    return GLib.SOURCE_CONTINUE;
                this._pasteSource = 0;
                return GLib.SOURCE_REMOVE;
            }
            this._pasteSource = 0;
            try {
                St.Clipboard.get_default().set_text(St.ClipboardType.CLIPBOARD, text);
                const time = GLib.get_monotonic_time();
                this._keyboard.notify_keyval(time, Clutter.KEY_Control_L, Clutter.KeyState.PRESSED);
                this._keyboard.notify_keyval(time, Clutter.KEY_v, Clutter.KeyState.PRESSED);
                this._keyboard.notify_keyval(time, Clutter.KEY_v, Clutter.KeyState.RELEASED);
                this._keyboard.notify_keyval(time, Clutter.KEY_Control_L, Clutter.KeyState.RELEASED);
            } catch (_error) {
                // Release keys even after a partially dispatched chord.
                const time = GLib.get_monotonic_time();
                this._keyboard.notify_keyval(time, Clutter.KEY_v, Clutter.KeyState.RELEASED);
                this._keyboard.notify_keyval(time, Clutter.KEY_Control_L, Clutter.KeyState.RELEASED);
                return GLib.SOURCE_REMOVE;
            }
            this._client.clearResult();
            return GLib.SOURCE_REMOVE;
        });
    }

    _effectiveSnapshot(snapshot) {
        return this._shortcutProblem
            ? {...snapshot, error: true, message: this._shortcutProblem}
            : snapshot;
    }

    _showShortcutProblem(message) {
        this._shortcutProblem = message;
        this._update(this._client?.snapshot ?? {
            available: false,
            state: 'unavailable',
            message: '',
            result: '',
            palette: '',
            error: false,
        });
    }

    _clearShortcutProblem() {
        if (!this._shortcutProblem)
            return;
        this._shortcutProblem = '';
        this._update(this._client?.snapshot ?? {
            available: false,
            state: 'unavailable',
            message: '',
            result: '',
            palette: '',
            error: false,
        });
    }

    _dismissOrCancel() {
        const hadShortcutProblem = Boolean(this._shortcutProblem);
        this._clearShortcutProblem();
        // A startup binding failure has no backend operation to cancel. Dismissing
        // it must not activate a missing application solely to send Cancel.
        if (!hadShortcutProblem || this._client?.snapshot.available)
            this._client?.request('Cancel');
    }
}
