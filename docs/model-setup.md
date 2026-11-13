# Model setup

LemonVox needs a separately installed and running inference server. Installing
LemonVox does not install that server, its inference backends or any models.
Configure its HTTP/HTTPS endpoint ending in `/v1` in Settings.

Choose a speech recognition model for transcription and a compatible text model
for cleanup. Settings distinguishes an empty selection, an uninstalled catalog
model, incompatible capabilities and an unreachable server. A downloaded model
does not need to be loaded into memory yet: loading on first use is normal and
can take time. Downloading files and loading existing files into memory are
different operations.

For a supported Lemonade server, refresh the catalog and choose the missing
model's setup action. Review the model name, available size estimate and server
destination before confirming the download. Files are downloaded by the server
onto the server's machine. Estimates may be unavailable; actual disk usage and
additional backend requirements depend on that server. Installation does not
change credentials or install the server itself.

Settings shows download progress and offers cancellation. A cancellation request
can take time to stop the server worker. Network failure or closing the client
does not prove that a server-owned job stopped; reconnect and inspect the server
before retrying. Disk-space and permission failures must be fixed on the server
machine. Existing settings remain available after failure. A successful download
is checked against refreshed inventory before its model is selected; save the
Settings draft to retain the selection. An unrelated model choice must not be
silently replaced.

For another OpenAI-compatible server, or a Lemonade version without verified
management support, install/configure models in that server's application or
administration interface. Confirm its transcription and chat capabilities, then
refresh LemonVox's catalog and select the installed model. Chat compatibility
alone does not establish transcription support.

## Management API compatibility

The model-management contract was inspected in the released Lemonade
[`v2026.46.2`](https://github.com/lemonade-sdk/lemonade/tree/v2026.46.2)
source. This is separate from the existing inference baseline (2026.44.1).

- `GET /v1/models?show_all=true` includes uninstalled catalog entries;
  `downloaded` describes files on disk.
- `GET /v1/health` reports the version and `all_models_loaded` separately.
- `POST /v1/pull` with `model_name`, `stream: true` and `subscribe: false`
  creates a server-owned download job.
- `GET /v1/downloads` reports jobs, progress and failures.
- `POST /v1/downloads/control` with a validated model job ID and `action: cancel`
  requests cancellation. A worker can still report `running: true` while stopping.

The client validates catalog membership and task capabilities before offering
installation. It sends structured JSON over HTTP; model names are never shell
commands. Health checks only read inventory. No setup flow uses `/load` to
install models: Lemonade may download missing files during loading, so a load
request must not bypass download consent.

References: [Lemonade lifecycle API](https://lemonade-server.ai/docs/api/lemonade/),
[released pull implementation](https://github.com/lemonade-sdk/lemonade/blob/v2026.46.2/src/cpp/server/routes/lemonade/models/pull.cpp),
[released catalog implementation](https://github.com/lemonade-sdk/lemonade/blob/v2026.46.2/src/cpp/server/routes/openai/models/models.cpp).
