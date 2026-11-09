// Local-only page and event collector for the private Wayland client matrix.
// It records only synthetic fixture state: focus, exact input, and submit.
import { createServer } from 'node:http';
import { appendFile, writeFile } from 'node:fs/promises';

const [readyFile, eventsFile] = process.argv.slice(2);
if (!readyFile || !eventsFile)
    throw new Error('ready-file and events-file arguments are required');

const allowedClients = new Set(['chrome', 'electron']);
const allowedEvents = new Set(['ready', 'focus', 'blur', 'input', 'submit']);

function pageFor(client) {
    const displayName = client[0].toUpperCase() + client.slice(1);
    const serializedClient = JSON.stringify(client);
    return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>LemonVox ${displayName} Fixture</title>
</head>
<body>
  <main>
    <h1>LemonVox ${displayName} Fixture</h1>
    <form id="target-form">
      <label for="target">Synthetic target field</label>
      <input id="target" name="target" autocomplete="off" autofocus>
    </form>
  </main>
  <script>
    const client = ${serializedClient};
    const target = document.getElementById('target');
    const form = document.getElementById('target-form');
    const post = (type, value = '') => {
      fetch('/event', {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        keepalive: true,
        body: JSON.stringify({ client, type, value }),
      }).catch(() => {});
    };
    target.addEventListener('focus', () => post('focus'));
    target.addEventListener('blur', () => post('blur'));
    target.addEventListener('input', () => post('input', target.value));
    form.addEventListener('submit', event => {
      event.preventDefault();
      post('submit', target.value);
    });
    const reportFocusedTarget = () => {
      target.focus();
      if (document.activeElement === target)
        post('focus');
    };
    window.addEventListener('focus', reportFocusedTarget);
    window.addEventListener('load', () => {
      reportFocusedTarget();
      post('ready');
    });
  </script>
</body>
</html>`;
}

function parseClient(requestUrl) {
    const client = new URL(requestUrl, 'http://127.0.0.1').searchParams.get('client');
    return allowedClients.has(client) ? client : null;
}

async function readRequest(request) {
    let body = '';
    for await (const chunk of request) {
        body += chunk;
        if (body.length > 4096)
            throw new Error('event request is too large');
    }
    return JSON.parse(body);
}

await writeFile(eventsFile, '');
const server = createServer(async (request, response) => {
    try {
        if (request.method === 'GET' && request.url.startsWith('/')) {
            const client = parseClient(request.url);
            if (!client) {
                response.writeHead(404).end();
                return;
            }
            response.writeHead(200, {
                'content-type': 'text/html; charset=utf-8',
                'cache-control': 'no-store',
            });
            response.end(pageFor(client));
            return;
        }
        if (request.method === 'POST' && request.url === '/event') {
            const event = await readRequest(request);
            if (!allowedClients.has(event.client) || !allowedEvents.has(event.type)
                || typeof event.value !== 'string') {
                response.writeHead(400).end();
                return;
            }
            await appendFile(eventsFile, `${JSON.stringify({
                client: event.client,
                type: event.type,
                value: event.value,
            })}\n`);
            response.writeHead(204).end();
            return;
        }
        response.writeHead(404).end();
    } catch (error) {
        response.writeHead(400).end(String(error));
    }
});

await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const address = server.address();
if (!address || typeof address === 'string')
    throw new Error('local event collector did not bind an IPv4 port');
await writeFile(readyFile, `http://127.0.0.1:${address.port}/\n`);

function close() {
    server.close(() => process.exit(0));
}
process.on('SIGTERM', close);
process.on('SIGINT', close);
