// Minimal Electron host used only by the private native matrix test.
// The renderer is the local HTTP fixture; it receives no Node integration.
const fs = require('node:fs');
const { app, BrowserWindow } = require('electron');

const mainScriptIndex = process.argv.indexOf(__filename);
const [fixtureUrl, readyFile, userDataDirectory] = mainScriptIndex === -1
    ? []
    : process.argv.slice(mainScriptIndex + 1);
if (!fixtureUrl || !readyFile || !userDataDirectory)
    throw new Error('fixture URL, ready-file, and user-data-directory arguments are required');

app.commandLine.appendSwitch('ozone-platform', 'wayland');
app.commandLine.appendSwitch('enable-features', 'UseOzonePlatform');
app.commandLine.appendSwitch('disable-background-networking');
app.commandLine.appendSwitch('disable-component-update');
fs.mkdirSync(userDataDirectory, {recursive: true});
app.setPath('userData', userDataDirectory);

let window;
app.whenReady().then(async () => {
    window = new BrowserWindow({
        show: true,
        width: 720,
        height: 360,
        autoHideMenuBar: true,
        webPreferences: {
            sandbox: true,
            contextIsolation: true,
            nodeIntegration: false,
        },
    });
    window.webContents.once('did-finish-load', () => fs.writeFileSync(readyFile, 'ready\n'));
    await window.loadURL(fixtureUrl);
});

app.on('window-all-closed', () => app.quit());
process.on('SIGTERM', () => app.quit());
process.on('SIGINT', () => app.quit());
