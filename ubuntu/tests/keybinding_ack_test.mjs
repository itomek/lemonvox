// Exercise the real binding adapter with a disposable compositor/settings boundary.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source = readFileSync(process.argv[2] || new URL('../extension/keybindings.js', import.meta.url), 'utf8')
    .replace(/^import .*;\n/gm, '').replace('export class ShortcutBindings', 'class ShortcutBindings');
const installed = new Map();
let reject = '';
let reserved = '';
const grabbed = new Map();
let nextAction = 2;
const settings = {
    values: {'toggle-shortcut': ['<Control><Alt>space'], 'cancel-shortcut': ['<Control><Super>Escape']},
    get_strv(key) { return [...this.values[key]]; },
    set_strv(key, value) { this.values[key] = [...value]; return true; },
    connect() { return 1; }, disconnect() {},
};
const context = vm.createContext({
    global: {display: {
        grab_accelerator(key) {
            if (key === reserved || [...grabbed.values()].includes(key)) return 0;
            const action = nextAction++; grabbed.set(action, key); return action;
        },
        ungrab_accelerator(action) { grabbed.delete(action); },
    }},
    Meta: {KeyBindingFlags: {IGNORE_AUTOREPEAT: 1}, KeyBindingAction: {NONE: 0}},
    Shell: {ActionMode: {NORMAL: 1}}, GLib: {source_remove() {}},
    Main: {wm: {
        addKeybinding(key, prefs) {
            if (prefs.get_strv(key).includes(reject)) return 0;
            installed.set(key, prefs.get_strv(key)); return 1;
        },
        removeKeybinding(key) { installed.delete(key); },
    }},
    validateShortcutPair(toggle, cancel) {
        return {valid: toggle[0] !== cancel[0], reason: 'Choose distinct shortcuts.'};
    },
});
vm.runInContext(source + '\nglobalThis.ShortcutBindings = ShortcutBindings;', context);
const bindings = new context.ShortcutBindings(settings, () => {}, () => {}, () => {});
assert.equal(bindings.setShortcuts('<Control>F9', '<Control>F10')[0], true);
assert.deepEqual(settings.values['toggle-shortcut'], ['<Control>F9']);
assert.equal(installed.has('toggle-shortcut'), true);
assert.equal(installed.has('cancel-shortcut'), false, 'idle acknowledgement releases cancellation');
reserved = '<Control><Alt>Escape';
assert.equal(bindings.setShortcuts(reserved, '<Control>F10')[0], false, 'reserved toggle must be rejected even when addKeybinding reports success');
assert.equal(bindings.setShortcuts('<Control>F9', reserved)[0], false, 'reserved cancellation must be rejected');
assert.deepEqual(settings.values['toggle-shortcut'], ['<Control>F9']);
assert.deepEqual(settings.values['cancel-shortcut'], ['<Control>F10']);
assert.equal(grabbed.size, 0, 'all successful probes must be released after partial failure');
reject = '<Control>F12';
assert.equal(bindings.setShortcuts('<Control>F11', reject)[0], false);
assert.deepEqual(settings.values['toggle-shortcut'], ['<Control>F9'], 'rejected pair restores prior toggle');
assert.deepEqual(settings.values['cancel-shortcut'], ['<Control>F10']);
assert.deepEqual(installed.get('toggle-shortcut'), ['<Control>F9']);
assert.equal(installed.has('cancel-shortcut'), false);
assert.equal(bindings.setShortcuts('<Control>F9', '<Control>F9')[0], false);
reject = '';
reserved = '<Control>F10';
bindings.setActive(true);
assert.equal(installed.has('toggle-shortcut'), true, 'active cancellation conflict preserves finish shortcut');
assert.equal(installed.has('cancel-shortcut'), false);
reserved = '';
bindings.setActive(false);
bindings.setActive(true);
assert.equal(bindings.setShortcuts('<Control>F1', '<Control>F2')[0], false, 'active dictation keeps its bindings');
assert.deepEqual(installed.get('toggle-shortcut'), ['<Control>F9']);
bindings.destroy();
assert.equal(installed.size, 0);
console.log('PASS: shortcut acknowledgement, conflict rollback, idle cancellation and active-session preservation');
