"""Collects every English UI string that goes through locale::T / locale::TF.
Usage: python extract_strings.py [out.json]   (default: strings.json next to this script)"""
import json, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.normpath(os.path.join(here, '..', '..', 'src'))
strings = []

def add(s):
    s = bytes(s, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')
    if s not in strings:
        strings.append(s)

lit = re.compile(r'"((?:[^"\\]|\\.)*)"')

def calls(text, name):
    """String literals inside name( ... ) calls, balanced parentheses."""
    for m in re.finditer(r'(?<![A-Za-z0-9_:])' + re.escape(name) + r'\(', text):
        i = m.end()
        depth = 1
        j = i
        while j < len(text) and depth:
            c = text[j]
            if c == '"':
                j += 1
                while j < len(text) and text[j] != '"':
                    j += 2 if text[j] == '\\' else 1
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            j += 1
        inner = text[i:j - 1]
        # only literals at this call level (skip nested calls like T(cfg::...))
        for lm in lit.finditer(inner):
            yield lm.group(1)

for f in ['ui/settings_window.cpp', 'app/app.cpp']:
    text = open(root + '\\' + f.replace('/', '\\'), encoding='utf-8').read()
    for name in ['T', 'locale::T', 'locale::TF']:
        for s in calls(text, name):
            add(s)
    if f == 'app/app.cpp':
        for m in re.finditer(r'add\(Id\w+, ([^;]+?)\);', text):
            for lm in lit.finditer(m.group(1)):
                add(lm.group(1))

cfg = open(root + r'\app\config.cpp', encoding='utf-8').read()
for m in re.finditer(r'\{ Metric::\w+,\s*Group::\w+,\s*"\w+",\s*"([^"]+)",\s*(nullptr|"([^"]+)")', cfg):
    add(m.group(1))
    if m.group(3):
        add(m.group(3))
gl = re.search(r'kGroupLabels\[\] = \{([^}]+)\}', cfg).group(1)
for lm in lit.finditer(gl):
    add(lm.group(1))

theme = open(root + r'\ui\theme.cpp', encoding='utf-8').read()
for m in re.finditer(r'\{ "(\w+)",\s+ImVec4', theme):
    add(m.group(1))

# Strings that are formatting only or proper names stay untranslated.
skip = {'%d%%', '15', '30', '60', '120', '0.25 s', '0.5 s', '1 s', '2 s', '5 s', '10 s', '30 s', '60 s',
        '##welcome'}
strings = [s for s in strings if s not in skip and not s.startswith('##')]
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'strings.json')
json.dump(strings, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print(len(strings), 'strings')
