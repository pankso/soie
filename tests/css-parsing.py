#!/usr/bin/env python3
# Run the css-parsing-tests corpus through tests/csstest and tests/cssvalue.
# Usage: css-parsing.py [-v] DIR
# The corpus follows the 2021 CSS Syntax draft; differences with the
# current draft that Soie implements are normalized away (see below).
import json, subprocess, sys

verbose = '-v' in sys.argv
corpus = [a for a in sys.argv[1:] if a != '-v'][0]

SYNTAX = ['component_value_list', 'one_component_value', 'declaration_list',
          'one_declaration', 'rule_list', 'one_rule', 'stylesheet',
          'blocks_contents']
VALUES = ['An+B', 'color_hexadecimal_3', 'color_hexadecimal_4',
          'color_keywords_3', 'color_keywords_4', 'color_hsl_3', 'color_hsl_4',
          'color_hwb_4', 'color_lab_4', 'color_lch_4', 'color_oklab_4',
          'color_oklch_4', 'color_function_4']
# color_functions_5 (device-cmyk, light-dark, custom profiles) is CSS
# Color 5: not implemented yet.

MATCH = {'~=': '~', '|=': '|', '^=': '^', '$=': '$', '*=': '*', '||': '|'}

def norm(x):
    """Current draft: no match tokens, no unicode-range token, and parse
    errors are not reported."""
    if isinstance(x, list):
        out = []
        for e in x:
            if isinstance(e, list) and e[:1] == ['error'] and \
               len(e) == 2 and e[1] not in ('bad-string', 'bad-url', ']',
                                            ')', '}', 'empty', 'invalid',
                                            'extra-input'):
                continue
            if isinstance(e, list) and e[:1] == ['error'] and \
               e[1] == 'invalid':
                continue
            if isinstance(e, str) and e in MATCH:
                out += [MATCH[e], e[1]]
                continue
            out.append(norm(e))
        return out
    if isinstance(x, float) and x == int(x):
        return int(x)
    return x

def trim_decl(x):
    """Current draft: declaration values lose surrounding whitespace."""
    if isinstance(x, list):
        if x[:1] == ['declaration'] and len(x) == 4:
            v = list(x[2])
            while v and v[0] == ' ':
                v.pop(0)
            while v and v[-1] == ' ':
                v.pop()
            return ['declaration', x[1], v, x[3]]
        return [trim_decl(e) for e in x]
    return x

# Inputs where the 2021 draft and the current one disagree.
KNOWN = {
    # "parse a declaration" consumed the whole input, ';' included
    ('one_declaration', 'foo:;'), ('one_declaration', 'foo:;bar:;'),
    # declaration lists could not hold qualified rules (nesting)
    ('declaration_list', '@ media screen { div{;}} a:b;; @media print{div{'),
}

def has_unicode_range(x):
    if isinstance(x, list):
        return x[:1] == ['unicode-range'] or any(map(has_unicode_range, x))
    return False

total_pass = total_fail = total_skip = 0
for name in SYNTAX + VALUES:
    try:
        tests = json.load(open('%s/%s.json' % (corpus, name)))
    except OSError:
        continue
    prog = './tests/csstest' if name in SYNTAX else './tests/cssvalue'
    inputs, expected = tests[0::2], tests[1::2]
    if name == 'stylesheet_bytes':
        continue
    data = b''
    for i in inputs:
        b = i.encode('utf-8', 'surrogatepass')
        data += b'%d\n' % len(b) + b
    out = subprocess.run([prog, name], input=data, stdout=subprocess.PIPE,
                         check=False).stdout.decode('utf-8', 'replace')
    lines = out.split('\n')
    ok = bad = skip = 0
    for k, (i, e) in enumerate(zip(inputs, expected)):
        if has_unicode_range(e) or (name, i) in KNOWN:
            skip += 1
            continue
        try:
            got = json.loads(lines[k])
        except (IndexError, ValueError):
            got = '<no output: %r>' % (lines[k] if k < len(lines) else '')
        if trim_decl(norm(got)) == trim_decl(norm(e)):
            ok += 1
        else:
            bad += 1
            if verbose:
                print('FAIL %s: %r\n  expected %s\n  got      %s' %
                      (name, i, json.dumps(e), json.dumps(got)))
    print('%-28s %5d passed %5d failed %4d skipped' % (name, ok, bad, skip))
    total_pass += ok
    total_fail += bad
    total_skip += skip
print('total: %d passed, %d failed, %d skipped' %
      (total_pass, total_fail, total_skip))
sys.exit(1 if total_fail else 0)
