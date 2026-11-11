// The policy module deliberately has no GNOME Shell imports, so this test runs
// in GJS without changing the user's desktop session.
import {
    ResultDelivery,
    activeState,
    barModel,
    normalizeBackendState,
    parsePalette,
    stateLabel,
    validateShortcutPair,
} from '../extension/policy.js';

function equal(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
}

function deepEqual(actual, expected, message) {
    const a = JSON.stringify(actual);
    const e = JSON.stringify(expected);
    if (a !== e)
        throw new Error(`${message}: expected ${e}, got ${a}`);
}

function testActiveStateNeverConsumesIdleCancel() {
    equal(activeState('recording'), true, 'recording accepts cancellation');
    equal(activeState('transcribing'), true, 'transcribing accepts cancellation');
    equal(activeState('idle'), false, 'idle preserves Escape delivery');
    equal(activeState('unknown'), false, 'unknown state preserves Escape delivery');
}

function testBarModelExposesOnlySafeActions() {
    deepEqual(barModel('idle', ''), {
        toggleLabel: 'Start dictation',
        showStop: false,
        showCancel: false,
        showCopy: false,
        showSettings: false,
        showDismiss: false,
    }, 'idle bar');
    deepEqual(barModel('recording', ''), {
        toggleLabel: 'Finish recording',
        showStop: true,
        showCancel: true,
        showCopy: false,
        showSettings: false,
        showDismiss: false,
    }, 'recording bar');
    deepEqual(barModel('idle', 'Copy this result'), {
        toggleLabel: 'Start dictation',
        showStop: false,
        showCancel: false,
        showCopy: true,
        showSettings: false,
        showDismiss: false,
    }, 'result enables only explicit Copy');
}

function testIdleErrorHasVisibleRecoveryWithoutIdleCancelBinding() {
    deepEqual(barModel('idle', '', true), {
        toggleLabel: 'Start dictation',
        showStop: false,
        showCancel: false,
        showCopy: false,
        showSettings: true,
        showDismiss: true,
    }, 'idle error exposes Settings and explicit dismiss only');
    equal(activeState('idle'), false, 'idle error must not claim the cancel shortcut');
}

function testInvalidBackendStateFailsClosed() {
    equal(normalizeBackendState('cleaning'), 'cleaning', 'known state passes through');
    equal(normalizeBackendState('completing'), 'unavailable', 'unknown state fails closed');
    equal(normalizeBackendState(null), 'unavailable', 'missing state fails closed');
}

function testStateLabelsNeverExposeDictatedResult() {
    equal(stateLabel('recording'), 'Recording', 'recording label');
    equal(stateLabel('transcribing'), 'Transcribing', 'transcribing label');
    equal(stateLabel('unavailable'), 'LemonVox unavailable', 'unavailable label');
}

function testConflictingShortcutsAreRejected() {
    deepEqual(validateShortcutPair(['<Control><Alt>space'], ['<Control><Super>Escape']), {
        valid: true,
        reason: '',
    }, 'distinct shortcuts work');
    deepEqual(validateShortcutPair(['<Control><Alt>space'], ['<Control><Alt>space']), {
        valid: false,
        reason: 'Dictation and cancellation shortcuts must differ.',
    }, 'same shortcut is rejected');
    deepEqual(validateShortcutPair([], ['<Control><Super>Escape']), {
        valid: false,
        reason: 'Choose a dictation shortcut.',
    }, 'empty dictation shortcut is rejected');
}

function testPaletteAcceptsOnlyFourNativeSemanticRgbaValues() {
    deepEqual(parsePalette('#101112ff #202122ff #303132ff #404142ff'), {
        windowBackground: '#101112ff',
        windowForeground: '#202122ff',
        cardBackground: '#303132ff',
        cardForeground: '#404142ff',
    }, 'valid native palette');
    equal(parsePalette('#101112ff #202122ff #303132ff'), null, 'incomplete palette falls back');
    equal(parsePalette('#101112ff red #303132ff #404142ff'), null, 'non-rgba palette falls back');
}

testActiveStateNeverConsumesIdleCancel();
testBarModelExposesOnlySafeActions();
testIdleErrorHasVisibleRecoveryWithoutIdleCancelBinding();
testInvalidBackendStateFailsClosed();
testStateLabelsNeverExposeDictatedResult();
testConflictingShortcutsAreRejected();
testPaletteAcceptsOnlyFourNativeSemanticRgbaValues();

print('PASS: GNOME Shell shortcut and bar policy');

const delivery = new ResultDelivery();
const snapshot = {available: true, state: 'idle', error: false, result: ''};
equal(delivery.take({...snapshot, result: 'old result'}), false, 'startup does not replay a retained result');
equal(delivery.take(snapshot), false, 'empty result is ignored');
equal(delivery.take({...snapshot, result: 'new result'}), true, 'new result is delivered');
equal(delivery.take(snapshot), false, 'optimistic ClearResult does not arm duplicate delivery');
equal(delivery.take({...snapshot, result: 'new result'}), false, 'repeated property change is ignored');
equal(delivery.take({...snapshot, state: 'recording'}), false, 'next recording arms delivery');
equal(delivery.take({...snapshot, result: 'new result'}), true, 'identical text in another recording is delivered');
equal(delivery.take({...snapshot, state: 'recording'}), false, 'active session does not paste');
equal(delivery.take({...snapshot, error: true, result: 'error result'}), false, 'error does not paste');
equal(delivery.take({...snapshot, available: false, result: 'offline result'}), false, 'offline result does not paste');
print('PASS: new result delivery is once per operation');
