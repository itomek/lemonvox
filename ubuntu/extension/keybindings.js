import Meta from 'gi://Meta';
import Shell from 'gi://Shell';
import GLib from 'gi://GLib';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';

import {validateShortcutPair} from './policy.js';

const TOGGLE_KEY = 'toggle-shortcut';
const CANCEL_KEY = 'cancel-shortcut';

// Main.wm manages compositor-native shortcuts. The cancellation binding exists
// only during a LemonVox operation, so its key is not consumed while idle.
export class ShortcutBindings {
    constructor(settings, onToggle, onCancel, onProblem, onReady = null) {
        this._settings = settings;
        this._onToggle = onToggle;
        this._onCancel = onCancel;
        this._onProblem = onProblem;
        this._onReady = onReady;
        this._active = false;
        this._toggleInstalled = false;
        this._cancelInstalled = false;
        this._restoring = false;
        this._rebindSource = 0;
        this._lastGood = this._readShortcuts();
        this._signals = [
            this._settings.connect(`changed::${TOGGLE_KEY}`, () => this._queueSettingsChanged()),
            this._settings.connect(`changed::${CANCEL_KEY}`, () => this._queueSettingsChanged()),
        ];
        if (this._install(this._lastGood))
            this._onReady?.();
    }

    setActive(active) {
        if (this._active === active)
            return;
        this._active = active;
        if (this._install(this._lastGood))
            this._onReady?.();
    }

    destroy() {
        if (this._rebindSource)
            GLib.source_remove(this._rebindSource);
        this._rebindSource = 0;
        for (const signal of this._signals)
            this._settings.disconnect(signal);
        this._signals = [];
        this._remove();
        this._settings = null;
        this._onToggle = null;
        this._onCancel = null;
        this._onProblem = null;
        this._onReady = null;
    }

    _readShortcuts() {
        return {
            toggle: this._settings.get_strv(TOGGLE_KEY),
            cancel: this._settings.get_strv(CANCEL_KEY),
        };
    }

    _queueSettingsChanged() {
        if (this._restoring || this._rebindSource)
            return;
        // Settings stores commonly apply both keys together. Coalescing the
        // notifications prevents a shortcut swap from being judged halfway.
        this._rebindSource = GLib.idle_add(GLib.PRIORITY_DEFAULT, () => {
            this._rebindSource = 0;
            this._settingsChanged();
            return GLib.SOURCE_REMOVE;
        });
    }

    _settingsChanged() {
        if (this._restoring)
            return;
        const candidate = this._readShortcuts();
        const valid = validateShortcutPair(candidate.toggle, candidate.cancel);
        if (!valid.valid) {
            this._restoreLastGood();
            this._onProblem?.(valid.reason);
            return;
        }
        const previous = this._lastGood;
        this._lastGood = candidate;
        if (!this._install(candidate)) {
            this._lastGood = previous;
            this._restoreLastGood();
        } else {
            this._onReady?.();
        }
    }

    _restoreLastGood() {
        this._restoring = true;
        this._settings.set_strv(TOGGLE_KEY, this._lastGood.toggle);
        this._settings.set_strv(CANCEL_KEY, this._lastGood.cancel);
        this._restoring = false;
        this._install(this._lastGood);
    }

    _install(shortcuts) {
        this._remove();
        try {
            const action = Main.wm.addKeybinding(
                TOGGLE_KEY,
                this._settings,
                Meta.KeyBindingFlags.IGNORE_AUTOREPEAT,
                Shell.ActionMode.NORMAL,
                () => this._onToggle?.());
            if (!action || action === Meta.KeyBindingAction.NONE)
                throw new Error('GNOME rejected the dictation shortcut');
            this._toggleInstalled = true;
        } catch (_error) {
            this._onProblem?.('The dictation shortcut could not be registered.');
            return false;
        }

        if (!this._active)
            return true;

        try {
            const action = Main.wm.addKeybinding(
                CANCEL_KEY,
                this._settings,
                Meta.KeyBindingFlags.IGNORE_AUTOREPEAT,
                Shell.ActionMode.NORMAL,
                () => this._onCancel?.());
            if (!action || action === Meta.KeyBindingAction.NONE)
                throw new Error('GNOME rejected the cancellation shortcut');
            this._cancelInstalled = true;
            return true;
        } catch (_error) {
            // Keep the finish shortcut working and require the visible X if
            // GNOME cannot claim the configured cancellation accelerator.
            this._onProblem?.('The cancellation shortcut could not be registered. Use Cancel.');
            return false;
        }
    }

    _remove() {
        if (this._toggleInstalled)
            Main.wm.removeKeybinding(TOGGLE_KEY);
        if (this._cancelInstalled)
            Main.wm.removeKeybinding(CANCEL_KEY);
        this._toggleInstalled = false;
        this._cancelInstalled = false;
    }
}
