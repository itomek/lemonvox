// A disposable Wayland GTK client for the headless Shell integration test.
// It records only Escape key delivery count; it never observes text input.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Gdk from 'gi://Gdk?version=4.0';
import Gtk from 'gi://Gtk?version=4.0';

const eventFile = ARGV[0];
const focusFile = ARGV[1];
const textFile = ARGV[2];
const activationFile = ARGV[3];
if (!eventFile || !focusFile || !textFile || !activationFile)
    throw new Error('fixture event, focus, text, and activation files are required');

let escapeCount = 0;
function record(value) {
    GLib.file_set_contents(eventFile, `${value}\n`);
}

function recordFocus(value) {
    GLib.file_set_contents(focusFile, `${value}\n`);
}

function recordText(value) {
    GLib.file_set_contents(textFile, value);
}

const app = new Gtk.Application({
    application_id: 'com.itomek.LemonVox.NativeFixture',
    flags: Gio.ApplicationFlags.NON_UNIQUE,
});

app.connect('activate', () => {
    const window = new Gtk.ApplicationWindow({
        application: app,
        title: 'LemonVox Native Fixture',
        default_width: 480,
        default_height: 180,
    });
    // The test target is intentionally empty: it proves focus retention without
    // placing dictated or user content in a screenshot or test artifact.
    const entry = new Gtk.Entry({
        placeholder_text: 'Synthetic target field',
        text: '',
        hexpand: true,
        margin_top: 48,
        margin_bottom: 48,
        margin_start: 64,
        margin_end: 64,
    });
    window.set_child(entry);
    const keyController = new Gtk.EventControllerKey();
    keyController.set_propagation_phase(Gtk.PropagationPhase.CAPTURE);
    keyController.connect('key-pressed', (_controller, keyval) => {
        if (keyval === Gdk.KEY_Escape) {
            escapeCount++;
            record(`escape:${escapeCount}`);
        }
        return false;
    });
    entry.add_controller(keyController);
    const focusController = new Gtk.EventControllerFocus();
    focusController.connect('enter', () => recordFocus('focused'));
    focusController.connect('leave', () => recordFocus('unfocused'));
    entry.add_controller(focusController);
    entry.connect('changed', () => recordText(entry.get_text()));
    entry.connect('activate', () => GLib.file_set_contents(activationFile, 'activated\n'));
    window.present();
    entry.grab_focus();
    recordText(entry.get_text());

    // A focused Gtk widget is not sufficient evidence that its Wayland
    // wl_keyboard has been announced. In the headless software-renderer path
    // that announcement can follow the initial surface/focus notifications.
    // Publish readiness only when both boundaries are complete, so callers
    // never race their first virtual key against seat setup.
    GLib.timeout_add(GLib.PRIORITY_DEFAULT, 10, () => {
        const keyboard = Gdk.Display.get_default()
            ?.get_default_seat()
            ?.get_keyboard();
        const active = window.is_active;
        if (active && keyboard) {
            record('ready');
            return GLib.SOURCE_REMOVE;
        }
        record(active ? 'waiting:keyboard' : 'waiting:window');
        return GLib.SOURCE_CONTINUE;
    });
});

app.run([]);
