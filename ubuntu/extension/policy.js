// Shell-independent policy. Keeping this module free of GNOME imports lets the
// safety decisions be exercised without registering a global shortcut.

const ACTIVE_STATES = new Set([
    'preparing',
    'recording',
    'transcribing',
    'cleaning',
    'inserting',
]);

const KNOWN_STATES = new Set(['idle', ...ACTIVE_STATES]);

export function activeState(state) {
    return ACTIVE_STATES.has(state);
}

export function normalizeBackendState(state) {
    return KNOWN_STATES.has(state) ? state : 'unavailable';
}

export function stateLabel(state) {
    switch (state) {
    case 'preparing':
        return 'Preparing';
    case 'recording':
        return 'Recording';
    case 'transcribing':
        return 'Transcribing';
    case 'cleaning':
        return 'Cleaning up text';
    case 'inserting':
        return 'Finishing';
    case 'idle':
        return 'Ready';
    default:
        return 'LemonVox unavailable';
    }
}

export function barModel(state, result, error = false) {
    const active = activeState(state);
    // Error recovery is deliberately distinct from cancellation. An idle
    // error must leave the configured cancellation accelerator untouched.
    const idleError = error === true && !active;
    return {
        toggleLabel: state === 'recording' ? 'Finish recording' : 'Start dictation',
        showStop: state === 'recording',
        showCancel: active,
        showCopy: !active && !idleError && typeof result === 'string' && result.length > 0,
        showSettings: idleError,
        showDismiss: idleError,
    };
}

export function validateShortcutPair(toggle, cancel) {
    if (!Array.isArray(toggle) || toggle.length === 0 || toggle.some(binding => !binding)) {
        return {valid: false, reason: 'Choose a dictation shortcut.'};
    }
    if (!Array.isArray(cancel) || cancel.length === 0 || cancel.some(binding => !binding)) {
        return {valid: false, reason: 'Choose a cancellation shortcut.'};
    }
    if (toggle.some(binding => cancel.includes(binding))) {
        return {valid: false, reason: 'Dictation and cancellation shortcuts must differ.'};
    }
    return {valid: true, reason: ''};
}

export function parsePalette(value) {
    if (typeof value !== 'string')
        return null;
    const colors = value.trim().split(/\s+/);
    if (colors.length !== 4 || colors.some(color => !/^#[0-9a-fA-F]{8}$/.test(color)))
        return null;
    return {
        windowBackground: colors[0],
        windowForeground: colors[1],
        cardBackground: colors[2],
        cardForeground: colors[3],
    };
}
