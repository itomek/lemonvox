# Cleanup execution and qualification

The cleanup prompt and request policy come from the shared C++ core. Factory
settings use **Lemonade · thinking off**: temperature zero, non-streaming output,
and `enable_thinking: false`. **Lemonade · server default** omits that extension.
**Compatible server · server default** also omits it, because other compatible
servers may reject Lemonade-specific fields. Select the execution mode explicitly
when changing server type. Existing saved configurations without an execution
mode retain compatible-server behavior.

Saved model selections and custom prompts remain intact. The previous built-in
prompt upgrades to the shared preservation prompt; Reset prompt uses that same
prompt. It asks for punctuation and removal of obvious fillers and immediate
accidental repetition, while preserving words, spoken numbers, names, negations,
clauses, language, tone, literal instructions, greetings and closings. A prompt
cannot guarantee a model's fidelity; qualification requires inspecting its output.

There is no automatically selected cleanup model. Gemma E2B remains a candidate
for qualification; the application does not download it or substitute it for an
explicit selection. An installed E4B selection can use thinking-off execution.
Missing models require server setup rather than arbitrary model substitution.

Generation allowance grows with UTF-8 transcript size, with punctuation headroom.
The shared API can accept a known context limit and rejects requests that cannot
fit conservatively instead of reducing their output allowance. Native settings
currently leave context unknown; the server enforces its effective context limit.
No global context, model residency, or server setting is changed. Empty or
truncated responses remain errors and are not inserted.

Debug diagnostics contain numeric request duration, token counts when available,
reasoning character count when supplied, and a fixed finish category. They do not
contain dictation, prompts, credentials or endpoint URLs. Missing usage fields
remain unknown. Total request time can include setup, queueing and generation;
logs do not assert that a request was cold/warm or that its wording was faithful.

## Synthetic qualification

`fixtures/cleanup/corpus.json` contains synthetic dictation covering the original
five cases, closings, Polish and mixed-language text, negation, identifiers,
amounts, ambiguous times and a long dictation. The offline evaluator checks that
words are retained in order, allowing case/punctuation, obvious fillers and
immediate repetitions to change. Digit-bearing identifiers retain their internal
punctuation; numeric signs, currency and percentage symbols also remain significant. The literal fixture also requires quotation and sentence punctuation.
These checks are deliberately conservative; a pass still needs human review for
language and tone.

Build the helper using the same request builder as the application:

```sh
bash scripts/test-core.sh --output /absolute/new/core-check
c++ -std=c++17 -Wall -Wextra -Werror -I core tests/cleanup_request_fixture.cpp \
  /absolute/new/core-check/liblemonvox_core.a -o /absolute/cleanup-request
python3 tests/cleanup_evaluation_test.py
```

To collect responses, explicitly choose a Lemonade endpoint and an already
installed, resident model. The collector refuses another model rather than
triggering model switching or downloads. Run it exclusively on an idle server;
a residency check cannot prevent another client changing that server concurrently.
Output paths must be new, and artifacts contain synthetic responses.

```sh
python3 scripts/collect-cleanup-evaluation.py \
  --endpoint http://127.0.0.1:13305/api/v1 \
  --model Gemma-4-E4B-it-GGUF \
  --request-tool /absolute/cleanup-request --output /absolute/new/responses.jsonl
python3 scripts/evaluate-cleanup.py /absolute/new/responses.jsonl
```

The collector records a first request separately, then runs the corpus sequentially
as warm requests. The first request is **not confirmed cold**: establishing cold
load latency requires a separately controlled server session. No unloading is
performed. The evaluator fails incomplete coverage, unfinished/empty responses,
reasoning, and wording changes. It reports missing reasoning-token fields as
unknown, and never prints response text. Response artifacts should be inspected
separately for language and tone. Repeated runs and measurements on each target
platform are required before choosing a universal default model.

The [Lemonade chat API](https://github.com/lemonade-sdk/lemonade/blob/main/docs/api/openai.md)
documents the thinking extension and generation parameters.
