import * as Extension from 'resource:///org/gnome/shell/extensions/extension.js';

import {BackendClient} from './dbus.js';
import {ShortcutBindings} from './keybindings.js';
import {activeState, normalizeBackendState} from './policy.js';
import {LemonVoxIndicator, SpeechBar} from './ui.js';

export default class LemonVoxExtension extends Extension.Extension {
    enable() {
        this._settings = this.getSettings();
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
