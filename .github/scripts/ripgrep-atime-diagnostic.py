"""Diagnostic only: a test-instrumented upstream tree is never a release input."""
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import subprocess
import tempfile
import time

repo = pathlib.Path(__file__).resolve().parents[2]
source = repo / '.cpp-build/atime-source'
evidence = repo / '.cpp-build/atime-evidence'
evidence.mkdir(parents=True, exist_ok=False)
pin = json.loads((repo / 'cpp-mcp/bundled-ripgrep.json').read_text())
assert platform.system() == 'Darwin' and platform.machine() == 'arm64'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip() == pin['source']
assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=source, text=True).strip()
temporary = pathlib.Path(tempfile.mkdtemp(prefix='devbox-rg-atime-', dir=repo.parent))
env = dict(os.environ, TMPDIR=str(temporary), TEMP=str(temporary), TMP=str(temporary), CARGO_INCREMENTAL='0')
env['CARGO_TARGET_DIR'] = str(repo / '.cpp-build/atime-target')
env['CARGO_ENCODED_RUSTFLAGS'] = '--remap-path-prefix=' + str(source) + '=/ripgrep'
target = 'aarch64-apple-darwin'
common = ['cargo', '+' + pin['rust'], '--locked', '--no-default-features', '--package', 'ripgrep', '--target', target, '--profile', pin['profile'], '--jobs', '2']

def cargo(action, args, name):
    command = common[:2] + [action] + common[2:] + args
    with (evidence / name).open('wb') as log:
        subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)

cargo('build', [], 'clean-build.log')
binary = pathlib.Path(env['CARGO_TARGET_DIR']) / target / pin['profile'] / 'rg'
binary_before = hashlib.sha256(binary.read_bytes()).hexdigest()
file = source / 'tests/misc.rs'
original = file.read_bytes()
needle = b'    dir.create(sub_dir.join("d"), "test");\n}'
assert original.count(needle) == 1
instrument = r'''
    let stamps: Vec<_> = ["a", "dir/c", "b", "dir/d"].iter().map(|name| {
        let metadata = std::fs::metadata(dir.path().join(name)).unwrap();
        let stamp = |value: std::io::Result<std::time::SystemTime>| {
            value.map(|v| v.duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos().to_string()).ok()
        };
        serde_json::json!({"path": name, "accessed": stamp(metadata.accessed()),
            "modified": stamp(metadata.modified()), "created": stamp(metadata.created())})
    }).collect();
    eprintln!("DEVBOX_ATIME {}", serde_json::json!({"root": dir.path(), "stamps": stamps}));
'''.replace(r'\"', '"').encode()
patched = original.replace(needle, needle[:-1] + instrument + b'}')
file.write_bytes(patched)
report = {'diagnosticOnly': True, 'releaseAllowed': False, 'platform': platform.platform(),
          'source': pin['source'], 'rust': pin['rust'], 'originalFixtureSha256': hashlib.sha256(original).hexdigest(),
          'instrumentedFixtureSha256': hashlib.sha256(patched).hexdigest(), 'binaryBefore': binary_before,
          'runs': [], 'temporary': str(temporary)}
try:
    cargo('test', ['--test', 'integration', '--no-run'], 'instrumented-build.log')
    candidates = [p for p in (binary.parent / 'deps').glob('integration-*') if p.is_file() and not p.suffix and os.access(p, os.X_OK)]
    assert len(candidates) == 1
    integration = candidates[0]
    for index in range(40):
        for test in ['misc::sort_accessed', 'misc::sortr_accessed']:
            result = subprocess.run([str(integration), test, '--exact', '--nocapture'], cwd=source, env=env, capture_output=True, timeout=10)
            output = (result.stdout + result.stderr).decode(errors='replace')
            records = [json.loads(line.split('DEVBOX_ATIME ', 1)[1]) for line in output.splitlines() if line.startswith('DEVBOX_ATIME ')]
            assert len(records) == 1
            stamps = records[0]['stamps']
            atimes = [int(row['accessed']) for row in stamps]
            row = {'index': index, 'test': test, 'exitCode': result.returncode,
                   'strictCreationOrder': all(a < b for a, b in zip(atimes, atimes[1:])),
                   'equalAccessTimes': len(set(atimes)) != len(atimes), 'stamps': stamps}
            report['runs'].append(row)
            if result.returncode or not row['strictCreationOrder']:
                (evidence / f'{index}-{test.replace("::", "-")}.log').write_text(output)
                print(json.dumps(row), flush=True)
        if index % 10 == 9:
            print(json.dumps({'iterations': index + 1, 'failures': sum(r['exitCode'] != 0 for r in report['runs'])}), flush=True)
    report['binaryAfter'] = hashlib.sha256(binary.read_bytes()).hexdigest()
    report['runtimeBinaryUnchanged'] = report['binaryBefore'] == report['binaryAfter']
finally:
    assert file.read_bytes() == patched
    file.write_bytes(original)
    report['finishedAtUtc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    (evidence / 'report.json').write_text(json.dumps(report, indent=2))
print(json.dumps({'completed': True, 'runs': len(report['runs']),
                 'failures': sum(r['exitCode'] != 0 for r in report['runs']),
                 'invalidTimestampPreconditions': sum(not r['strictCreationOrder'] for r in report['runs'])}), flush=True)
