"""Writes locales/<code>.json from strings.json (made by extract_strings.py) and the
tr/<code>.py tables. Checks that every string is translated and that printf placeholders survive.
Usage: python gen_locales.py [lang ...]"""
import importlib.util, json, os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.normpath(os.path.join(here, '..', '..'))
strings = json.load(open(os.path.join(here, 'strings.json'), encoding='utf-8'))
untranslated = {'LibreHardwareMonitor'}     # proper names
langs = ['de', 'fr', 'es', 'it', 'nl', 'pt-BR', 'pt-PT', 'pl', 'tr', 'ru', 'ja', 'ko', 'zh-CN', 'ar', 'fa']
only = sys.argv[1:]

placeholder = re.compile(r'%[-+#0-9.]*[dsfu]')

def write(code, name, table):
    data = {
        'meta': {
            'app': 'FPS Overlay',
            'app_version': '2.0.0',
            'language': code,
            'language_name': name,
            'note': 'Keys are the English source strings. Missing keys fall back to English.',
        },
        'translations': {'ui': table},
    }
    path = os.path.join(repo, 'locales', code + '.json')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write('\n')

problems = 0
write('en-US', 'English', {s: s for s in strings})
for code in langs:
    if only and code not in only:
        continue
    path = os.path.join(here, 'tr', code.replace('-', '_') + '.py')
    if not os.path.exists(path):
        print(code, 'MISSING FILE')
        problems += 1
        continue
    spec = importlib.util.spec_from_file_location(code, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    table = {}
    for s in strings:
        if s in untranslated:
            continue
        t = mod.T.get(s)
        if not t:
            print(code, 'missing:', s)
            problems += 1
            continue
        if sorted(placeholder.findall(s)) != sorted(placeholder.findall(t)):
            print(code, 'placeholder mismatch:', s, '->', t)
            problems += 1
            continue
        table[s] = t
    extra = set(mod.T) - set(strings)
    for e in sorted(extra):
        print(code, 'unused key:', e)
    write(code, mod.NAME, table)
    print(code, len(table), 'strings')
print('problems:', problems)
sys.exit(1 if problems else 0)
