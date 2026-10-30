import argparse
import hashlib
import json
from pathlib import Path


def wire_identity(file):
    digest = hashlib.sha256()
    count = 0
    with file.open('r', encoding='utf-8') as stream:
        for number, line in enumerate(stream):
            if number == 0:
                continue
            if line.rstrip('\r\n') == '{fixture-invalid':
                value = '{fixture-invalid'
            else:
                value = json.loads(line)
                if value.get('type') == 'events':
                    for event in value['events']:
                        event.pop('observedAt')
                value = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'))
            digest.update(value.encode('utf-8') + b'\n')
            count += 1
    return dict(records=count, sha256=digest.hexdigest(), excludedFields=['initial ready frame; outside burst workload', 'events[*].observedAt; fixture clock only'])


def compare(baseline, optimized):
    before = json.loads((baseline / 'result.json').read_text('utf-8'))
    after = json.loads((optimized / 'result.json').read_text('utf-8'))
    if before['status'] != 'baseline-measured' or after['status'] != 'passed':
        raise ValueError('Completed baseline and strict optimized verification are required')
    a, b = before['measurements'], after['measurements']
    for key in ('edition', 'transportMessages', 'giantValueRetained', 'queuedBeforeDrain', 'pumpCalls', 'archiveEntries', 'transportSequenceRetained', 'drainMode'):
        if a[key] != b[key]:
            raise ValueError('Workload/behavior differs: ' + key)
    original = wire_identity(baseline / 'fixture-wire.jsonl')
    updated = wire_identity(optimized / 'fixture-wire.jsonl')
    if original != updated or original['records'] != 361:
        raise ValueError('Full producer workload differs beyond declared fixture-clock fields')
    queue = b['queueAfterExit']
    if (not b['exitMarkerObserved'] or b['pendingOnClose'] != 0 or queue['enqueuedMessages'] != 362
            or queue['dequeuedMessages'] != 362 or queue['pendingUtf16Bytes'] != 0
            or not queue['stdoutEnded'] or not queue['stderrEnded']):
        raise ValueError('Optimized close lost a protocol/control line or EOF boundary')
    if not b['sourceWireUnchanged'] or len(a['acknowledgmentMilliseconds']) != 4 or len(b['acknowledgmentMilliseconds']) != 4:
        raise ValueError('Control order or raw-wire integrity verification differs')
    if b['rowsBuilt'] >= a['rowsBuilt'] / 10:
        raise ValueError('Measured row-rebuild work was not substantially reduced')
    return dict(protocol='v8trace/ui-queue-comparison/1', edition=b['edition'], conclusion='PASS',
                sourceWorkload=original, baselineUiSha256=before['uiSha256'], optimizedUiSha256=after['uiSha256'],
                rowsBuilt=dict(before=a['rowsBuilt'], after=b['rowsBuilt'], ratio=a['rowsBuilt'] / b['rowsBuilt']),
                burstWallMilliseconds=dict(before=a['wallMilliseconds'], after=b['wallMilliseconds']),
                processCpuMilliseconds=dict(before=a['cpuMilliseconds'], after=b['cpuMilliseconds']),
                stopReplyMilliseconds=dict(before=a['stopReplyMilliseconds'], after=b['stopReplyMilliseconds']),
                shutdownMilliseconds=dict(before=a['shutdownMilliseconds'], after=b['shutdownMilliseconds']),
                baselineExit=dict(finalReplyObserved=a['exitMarkerObserved'], pendingMessages=a['pendingOnClose']),
                optimizedExit=dict(finalReplyObserved=True, enqueued=362, dequeued=362, pendingMessages=0, bothEofsObserved=True),
                scope='local owned protocol fixtures; not browser/site performance; no normal 200ms waits during burst drain')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--optimized', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = compare(args.baseline, args.optimized)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    print(json.dumps(result, ensure_ascii=True))


if __name__ == '__main__':
    main()
