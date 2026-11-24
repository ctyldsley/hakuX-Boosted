#!/usr/bin/env python3
# hakuX-Boosted: the differential tester's bookkeeping and report.
#
#   dt_report.py merge <state.json> <batch.jsonl>
#       adds one batch's mismatches (the runner's JSON lines) to the run's
#       state: counts for every group, the first examples of each in full
#   dt_report.py render <state.json> <known-issues.txt> <out dir>
#       writes report.md, regression-candidates.txt and summary.json to the
#       directory; exit status 1 if anything is not in known-issues.txt
#
# A group is a single instruction's key ("addcx+oe": mnemonic, +oe and +rc
# when set) or "seq" for a sequence, with the kinds of state that differed.
import json
import os
import sys

EXAMPLES = 3


def merge(state_path, batch_path):
    state = {'stats': {}, 'groups': {}, 'batches': []}
    if os.path.exists(state_path):
        state = json.load(open(state_path))
    for line in open(batch_path):
        line = line.strip()
        if not line:
            continue
        j = json.loads(line)
        if 'stats' in j:
            state['batches'].append([j['seed'], j['tier'], j['start'], j['count']])
            for cls, (cases, bad) in j['classes'].items():
                s = state['stats'].setdefault(cls, [0, 0])
                s[0] += cases
                s[1] += bad
            continue
        kinds = ','.join(sorted(set(d['kind'] for d in j['diff'])))
        gid = '%d|%s|%s' % (j['tier'], j['key'], kinds)
        g = state['groups'].setdefault(gid, {'count': 0, 'examples': []})
        g['count'] += 1
        if len(g['examples']) < EXAMPLES and 'code' in j:
            g['examples'].append(j)
    json.dump(state, open(state_path, 'w'))


def load_known(path):
    """key -> {kind: reason}; the key '*' applies to every instruction."""
    known = {}
    for line in open(path):
        reason = line.split('#', 1)[1].strip() if '#' in line else ''
        tok = line.split('#')[0].split()
        if len(tok) < 2:
            continue
        for kind in tok[1].split(','):
            known.setdefault(tok[0], {})[kind] = reason
    return known


def explain(known, tier, key, kinds):
    """The reasons if every kind of the group is known, else None."""
    if tier != 1:
        return None
    reasons = []
    for kind in kinds.split(','):
        r = None
        for k in (key, key.split('+')[0], '*'):
            if k in known and (kind in known[k] or '*' in known[k]):
                r = known[k].get(kind, known[k].get('*'))
                break
        if r is None:
            return None
        if r not in reasons:
            reasons.append(r)
    return reasons


def regression_line(name, j):
    parts = [name, 'code=' + ','.join(j['code'])]
    if j['skip'].get('floatvec'):
        parts.append('floatvec=1')
    parts.append('in:')
    parts += ['%s=%s' % kv for kv in j['in'].items()]
    parts.append('out:')
    parts += ['%s=%s' % kv for kv in j['refout'].items()]
    sk = j['skip']
    skip = []
    if int(sk['gprhi'], 16):
        skip.append('gprhi=' + sk['gprhi'])
    if int(sk['fprhi'], 16):
        skip.append('fprhi=' + sk['fprhi'])
    if sk['crmask'] != 'ffffffff':
        skip.append('crmask=' + sk['crmask'])
    if sk.get('vscrmask', '00010001') != '00010001':
        skip.append('vscrmask=' + sk['vscrmask'])
    if sk['tol']:
        skip.append('tol=1')
    if sk.get('nanequal'):
        skip.append('nanequal=1')
    skip += ['v%d' % v for v in sk['vr']]
    if skip:
        parts += ['skip:'] + skip
    return ' '.join(parts)


def example_text(j):
    out = ['seed %d, tier %d, case %d' % (j['seed'], j['tier'], j['index']), '']
    for word, text in zip(j['code'], j['asm']):
        out.append('    %s  %s' % (word, ' '.join(text.split())))
    ins = ['%s=%s' % (k, v) for k, v in j['in'].items() if k != 'mem']
    out.append('    in:   ' + ' '.join(ins))
    out.append('    ours: ' + ' '.join('%s=%s' % (d['name'], d['ours']) for d in j['diff']))
    out.append('    ref:  ' + ' '.join('%s=%s' % (d['name'], d['ref']) for d in j['diff']))
    return out


def render(state_path, known_path, out_dir):
    state = json.load(open(state_path)) if os.path.exists(state_path) else {
        'stats': {}, 'groups': {}, 'batches': []}
    known = load_known(known_path)
    new, old = [], []
    for gid, g in state['groups'].items():
        tier, key, kinds = gid.split('|')
        reasons = explain(known, int(tier), key, kinds)
        (old if reasons is not None else new).append((key, kinds, int(tier), g, reasons))
    new.sort(key=lambda x: (x[2], x[0], x[1]))
    old.sort(key=lambda x: (x[0], x[1]))
    failures = []
    for name in ('failures.txt',):
        p = os.path.join(out_dir, name)
        if os.path.exists(p):
            failures = [l.rstrip() for l in open(p) if l.strip()]
    regress = []
    p = os.path.join(out_dir, 'regressions.log')
    if os.path.exists(p):
        regress = [l.rstrip() for l in open(p)]
    regress_failed = [l for l in regress if l.startswith('FAIL')]
    total = sum(s[0] for s in state['stats'].values())
    seeds = sorted(set(b[0] for b in state['batches']))

    md = ['# PowerPC to ARM64 translator: differential test', '']
    verdict = 'no new difference' if not (new or failures or regress_failed) else 'NEW DIFFERENCES'
    md.append('**%s.** %d cases (%d batches, seeds %s) through the ARM64 back end and the reference '
              '(QEMU, PowerPC 970, 32-bit mode); %d groups differ that are not in `known-issues.txt`, '
              '%d that are; %d fixed cases of `regressions.txt` run, %d failed; %d batches did not complete.'
              % (verdict, total, len(state['batches']),
                 ('%d to %d' % (seeds[0], seeds[-1])) if seeds else 'none',
                 len(new), len(old), len([l for l in regress if l.startswith(('ok', 'FAIL'))]),
                 len(regress_failed), len(failures)))
    md += ['', '| Class | Cases | Differ |', '|---|---:|---:|']
    for cls, (cases, bad) in state['stats'].items():
        if cases:
            md.append('| %s | %d | %d |' % (cls, cases, bad))
    md += ['', 'A class is one instruction a case (`seq`: sequences of two to six with forward branches). '
           '"Differ" counts known differences too.', '']

    candidates = []
    if failures:
        md += ['## Batches that did not complete', '']
        md += ['- `%s`' % f for f in failures] + ['']
    if regress_failed:
        md += ['## Fixed cases that fail again', '', '```'] + [l for l in regress if not l.startswith('ok')] + ['```', '']
    md += ['## New differences', '']
    if not new:
        md += ['None.', '']
    for key, kinds, tier, g, _ in new:
        md += ['### `%s`: %s (%d cases)' % (key, kinds, g['count']), '']
        for n, j in enumerate(g['examples']):
            md += example_text(j) + ['']
            candidates.append(regression_line('%s_%s_s%dt%di%d' % (
                key.replace('+', '_').replace(':', '_'), kinds.replace(',', '_').replace('.', ''),
                j['seed'], j['tier'], j['index']), j))
    md += ['## Known differences seen in this run', '',
           'Each is in `known-issues.txt` with its reason; `boosted/notes/jit-difftest.md` says more.', '',
           '| Instruction | State that differs | Cases | Reason |', '|---|---|---:|---|']
    for key, kinds, tier, g, reasons in old:
        md.append('| `%s` | %s | %d | %s |' % (key, kinds, g['count'], '; '.join(reasons)))
    md += ['', '### One example of each', '']
    for key, kinds, tier, g, reasons in old:
        if not g['examples']:
            continue
        md += ['`%s`: %s' % (key, kinds), ''] + example_text(g['examples'][0]) + ['']
    seen = set(k for k, _, _, _, _ in old)
    unseen = sorted(k for k in known if k != '*' and k not in seen and k.split('+')[0] not in
                    set(s.split('+')[0] for s in seen))
    if unseen and total >= 200000:
        md += ['## Entries of `known-issues.txt` not seen in this run', '',
               'Either fixed or too rare for this many cases: ' + ', '.join('`%s`' % u for u in unseen), '']
    if regress:
        md += ['## Fixed cases (`regressions.txt`)', '', '```'] + regress + ['```', '']
    open(os.path.join(out_dir, 'report.md'), 'w').write('\n'.join(md) + '\n')
    open(os.path.join(out_dir, 'regression-candidates.txt'), 'w').write(
        ''.join(c + '\n' for c in candidates))
    summary = {'cases': total, 'new_groups': len(new), 'known_groups': len(old),
               'failed_batches': len(failures), 'failed_regressions': len(regress_failed)}
    json.dump(summary, open(os.path.join(out_dir, 'summary.json'), 'w'))
    print(json.dumps(summary))
    return 1 if (new or failures or regress_failed) else 0


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'merge':
        merge(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 5 and sys.argv[1] == 'render':
        sys.exit(render(sys.argv[2], sys.argv[3], sys.argv[4]))
    else:
        sys.exit(__doc__ or 'usage: dt_report.py merge|render ...')
