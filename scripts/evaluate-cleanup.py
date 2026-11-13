#!/usr/bin/env python3
"""Offline, conservative qualification gate for synthetic cleanup responses.

Consumes JSONL: {fixture, phase: first_request|warm, elapsed_ms, response: <API JSON>}.
Never sends requests or changes model residency. Reports IDs and metrics, not text.
A pass is a mechanical preservation check, not language/tone certification.
"""
import argparse
import json
import math
from pathlib import Path
import re
import statistics
import unicodedata

CORPUS = Path(__file__).resolve().parents[1] / 'fixtures/cleanup/corpus.json'
FILLERS = {'um', 'uh', 'yyy'}


def words(text):
    # Preserve identifier punctuation and decimal separators within words.
    tokens = re.findall(r"[.,]\d+|\w+(?:[-.,/:'’]\w+)*|[^\w\s]", text.casefold(), re.UNICODE)
    tokens = [token for token in tokens if re.search(r"\w", token)
              or token in "%٪‰‱+−-°#=<>≤≥"
              or unicodedata.category(token) == "Sc"]
    # Word hyphenation is punctuation; digit-bearing identifiers retain it.
    return [part for token in tokens for part in (token.split("-") if not any(c.isdigit() for c in token) else [token])]


def preserves(source, cleaned):
    """Only punctuation/case, obvious fillers, immediate repeats may change."""
    before, after = words(source), words(cleaned)
    reachable = {0}
    for index, token in enumerate(before):
        next_positions = set()
        for position in reachable:
            if position < len(after) and after[position] == token:
                next_positions.add(position + 1)
            if token in FILLERS or (index and before[index - 1] == token):
                next_positions.add(position)
        reachable = next_positions
    return len(after) in reachable


def evaluate(fixture, sample):
    issues = []
    response = sample.get('response', {})
    choices = response.get('choices', [])
    choice = choices[0] if choices and isinstance(choices[0], dict) else {}
    message = choice.get('message', {})
    content = message.get('content')
    finish = choice.get('finish_reason')
    if finish != 'stop':
        issues.append('unfinished')
    if not isinstance(content, str) or not content.strip():
        issues.append('empty')
    elif not preserves(fixture['transcript'], content):
        issues.append('changed_words')
    if fixture.get('require_punctuation') and isinstance(content, str):
        # A literal quotation needs boundaries and the trailing dictated sentence.
        if not re.search(r'[.!?][\s\"”]+(?:this|This)\b', content):
            issues.append('literal_sentence_punctuation')
        if not any(mark in content for mark in ('"', '“', '«')):
            issues.append('literal_quote_punctuation')
    reasoning = message.get('reasoning_content') or message.get('reasoning')
    if reasoning:
        issues.append('reasoning_present')
    usage = response.get('usage') or {}
    details = usage.get('completion_tokens_details') or {}
    reasoning_tokens = details.get('reasoning_tokens')
    if reasoning_tokens not in (None, 0):
        issues.append('reasoning_tokens')
    elapsed = sample.get('elapsed_ms')
    if isinstance(elapsed, bool) or not isinstance(elapsed, (int, float)) or not math.isfinite(elapsed) or elapsed < 0:
        issues.append('invalid_elapsed_ms')
        elapsed = None
    phase = sample.get('phase')
    if phase not in ('first_request', 'warm'):
        issues.append('invalid_phase')
    def count(value):
        return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None
    return {'fixture': fixture['id'], 'phase': phase if phase in ('first_request', 'warm') else 'unknown', 'elapsed_ms': elapsed,
            'completion_tokens': count(usage.get('completion_tokens')), 'reasoning_tokens': count(reasoning_tokens),
            'reasoning_characters': len(reasoning) if isinstance(reasoning, str) else None,
            'finish_reason': finish if finish in ('stop', 'length', 'tool_calls', 'content_filter') else 'unknown',
            'fidelity': 'fail' if issues else 'pass', 'issues': issues}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('responses', type=Path)
    args = parser.parse_args()
    corpus = {row['id']: row for row in json.loads(CORPUS.read_text())}
    results = []
    unknown = False
    try:
        for line in args.responses.read_text().splitlines():
            sample = json.loads(line)
            fixture = corpus.get(sample.get('fixture'))
            if fixture is None:
                unknown = True
                continue
            results.append(evaluate(fixture, sample))
    except (ValueError, TypeError, AttributeError, OSError):
        parser.exit(2, 'Invalid response artifact; details omitted to protect input contents.\n')
    warm_seen = {row['fixture'] for row in results if row['phase'] == 'warm'}
    first_seen = {row['fixture'] for row in results if row['phase'] == 'first_request'}
    missing_warm = sorted(set(corpus) - warm_seen)
    # First request includes possible load/setup; never label it confirmed cold.
    summary = {'samples': results, 'missing_warm_fixtures': missing_warm,
               'first_request_observed': bool(first_seen), 'unknown_fixture': unknown,
               'qualification': 'mechanical_only_human_language_and_tone_review_required'}
    for phase in ('first_request', 'warm'):
        timings = [row['elapsed_ms'] for row in results if row['phase'] == phase and row['elapsed_ms'] is not None]
        summary[phase + '_median_ms'] = statistics.median(timings) if timings else None
    print(json.dumps(summary, ensure_ascii=True, indent=2))
    return int(unknown or bool(missing_warm) or not first_seen or any(row['issues'] for row in results))


if __name__ == '__main__':
    raise SystemExit(main())
