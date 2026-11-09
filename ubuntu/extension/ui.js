import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import St from 'gi://St';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';

import {activeState, barModel, normalizeBackendState, parsePalette, stateLabel} from './policy.js';

const CLIPBOARD = St.Clipboard.get_default();

function copyResult(snapshot, onCopied) {
    if (typeof snapshot.result !== 'string' || snapshot.result.length === 0)
        return;
    try {
        CLIPBOARD.set_text(St.ClipboardType.CLIPBOARD, snapshot.result);
    } catch (_error) {
        return;
    }
    // Never show dictated content in the Shell. The backend owns deletion after
    // the explicit clipboard transfer succeeds.
    onCopied();
}

function displayText(snapshot) {
    const state = normalizeBackendState(snapshot.state);
    return snapshot.message?.trim() || stateLabel(state);
}

function semanticButton(label, accessibleName, handler) {
    const button = new St.Button({
        label,
        can_focus: false,
        reactive: true,
        track_hover: true,
        style_class: 'button',
        accessible_name: accessibleName,
    });
    button.connect('clicked', handler);
    return button;
}

function applySemanticPalette(actor, label, buttons, serializedPalette) {
    const palette = parsePalette(serializedPalette);
    if (!palette) {
        actor.set_style(null);
        label.set_style(null);
        for (const button of buttons)
            button.set_style(null);
        return;
    }
    actor.set_style(`background-color: ${palette.windowBackground}; color: ${palette.windowForeground};`);
    label.set_style(`color: ${palette.windowForeground};`);
    for (const button of buttons) {
        button.set_style(`background-color: ${palette.cardBackground}; color: ${palette.cardForeground};`);
    }
}

export class SpeechBar {
    constructor(actions) {
        this._actions = actions;
        this._snapshot = {state: 'unavailable', message: '', result: '', palette: '', error: false};
        this.actor = new St.BoxLayout({
            // popup-menu-content supplies the active Shell theme's semantic
            // surface, contrast, border and shadow on both Yaru light/dark.
            style_class: 'popup-menu-content lemonvox-bar',
            reactive: false,
            can_focus: false,
            accessible_name: 'LemonVox dictation controls',
        });
        this._label = new St.Label({style_class: 'lemonvox-status', y_align: Clutter.ActorAlign.CENTER});
        this._stop = semanticButton('Stop', 'Stop recording', () => this._actions.stop());
        this._cancel = semanticButton('×', 'Cancel dictation', () => this._actions.cancel());
        this._copy = semanticButton('Copy', 'Copy result', () => copyResult(this._snapshot, this._actions.clearResult));
        this._settings = semanticButton('Settings', 'Open LemonVox settings', () => this._actions.settings());
        this.actor.add_child(this._label);
        this.actor.add_child(this._stop);
        this.actor.add_child(this._cancel);
        this.actor.add_child(this._copy);
        this.actor.add_child(this._settings);
        Main.layoutManager.addChrome(this.actor, {affectsInputRegion: true, trackFullscreen: true});
        this._monitorSignal = Main.layoutManager.connect('monitors-changed', () => this._position());
        this._positionSource = 0;
        this.actor.connect('notify::allocation', () => this._position());
        this.actor.hide();
    }

    update(snapshot) {
        this._snapshot = snapshot;
        const state = normalizeBackendState(snapshot.state);
        const model = barModel(state, snapshot.result, snapshot.error);
        this._label.text = displayText(snapshot);
        this._stop.visible = model.showStop;
        this._cancel.visible = model.showCancel || model.showDismiss;
        this._cancel.accessible_name = model.showCancel ? 'Cancel dictation' : 'Dismiss error';
        this._copy.visible = model.showCopy;
        this._settings.visible = model.showSettings;
        applySemanticPalette(this.actor, this._label,
            [this._stop, this._cancel, this._copy, this._settings], snapshot.palette);
        this.actor.visible = activeState(state) || model.showCopy || model.showDismiss;
        if (this._positionSource) {
            GLib.source_remove(this._positionSource);
            this._positionSource = 0;
        }
        if (this.actor.visible) {
            this._positionSource = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
                this._positionSource = 0;
                this._position();
                return GLib.SOURCE_REMOVE;
            });
        }
    }

    destroy() {
        if (this._monitorSignal)
            Main.layoutManager.disconnect(this._monitorSignal);
        this._monitorSignal = 0;
        if (this._positionSource)
            GLib.source_remove(this._positionSource);
        this._positionSource = 0;
        Main.layoutManager.removeChrome(this.actor);
        this.actor.destroy();
        this._actions = null;
    }

    _position() {
        const monitor = Main.layoutManager.primaryMonitor;
        if (!monitor || !this.actor.visible)
            return;
        const margin = 24;
        const x = Math.round(monitor.x + (monitor.width - this.actor.width) / 2);
        const y = Math.round(monitor.y + monitor.height - this.actor.height - margin);
        this.actor.set_position(x, y);
    }
}

export class LemonVoxIndicator {
    constructor(actions) {
        this._actions = actions;
        this._snapshot = {state: 'unavailable', message: '', result: '', error: false};
        this._indicator = new PanelMenu.Button(0.0, 'LemonVox', false);
        this._indicator.add_child(new St.Icon({
            icon_name: 'microphone-sensitivity-high-symbolic',
            style_class: 'system-status-icon',
        }));
        this._status = new PopupMenu.PopupMenuItem('LemonVox unavailable', {reactive: false});
        this._toggle = new PopupMenu.PopupMenuItem('Start dictation');
        this._stop = new PopupMenu.PopupMenuItem('Stop recording');
        this._cancel = new PopupMenu.PopupMenuItem('Cancel');
        this._copy = new PopupMenu.PopupMenuItem('Copy result');
        this._settings = new PopupMenu.PopupMenuItem('Settings');
        this._quit = new PopupMenu.PopupMenuItem('Quit');
        this._toggle.connect('activate', () => this._actions.toggle());
        this._stop.connect('activate', () => this._actions.stop());
        this._cancel.connect('activate', () => this._actions.cancel());
        this._copy.connect('activate', () => copyResult(this._snapshot, this._actions.clearResult));
        this._settings.connect('activate', () => this._actions.settings());
        this._quit.connect('activate', () => this._actions.quit());
        this._indicator.menu.addMenuItem(this._status);
        this._indicator.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._indicator.menu.addMenuItem(this._toggle);
        this._indicator.menu.addMenuItem(this._stop);
        this._indicator.menu.addMenuItem(this._cancel);
        this._indicator.menu.addMenuItem(this._copy);
        this._indicator.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._indicator.menu.addMenuItem(this._settings);
        this._indicator.menu.addMenuItem(this._quit);
        Main.panel.addToStatusArea('lemonvox', this._indicator);
    }

    update(snapshot) {
        this._snapshot = snapshot;
        const state = normalizeBackendState(snapshot.state);
        const model = barModel(state, snapshot.result, snapshot.error);
        this._status.label.text = displayText(snapshot);
        this._toggle.label.text = model.toggleLabel;
        this._stop.visible = model.showStop;
        this._cancel.visible = model.showCancel;
        this._copy.visible = model.showCopy;
        // Explicit requests are allowed to activate the D-Bus service. Other
        // controls require a live backend because their state is ambiguous.
        this._toggle.setSensitive(true);
        this._stop.setSensitive(snapshot.available && model.showStop);
        this._cancel.setSensitive(snapshot.available && model.showCancel);
        this._copy.setSensitive(model.showCopy);
        this._settings.setSensitive(true);
        this._quit.setSensitive(snapshot.available);
    }

    destroy() {
        this._indicator.destroy();
        this._actions = null;
    }
}
