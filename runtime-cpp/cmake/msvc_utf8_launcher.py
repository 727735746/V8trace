import os
from pathlib import Path
import subprocess
import sys


def normalize_line(line, prefixes, legacy_encoding=None):
    try:
        decoded = line.decode('utf-8')
    except UnicodeDecodeError:
        decoded = line.decode(legacy_encoding or ('mbcs' if os.name == 'nt' else 'utf-8'), errors='replace')
    for prefix in prefixes:
        if prefix and decoded.startswith(prefix):
            return ('V8TRACE_INCLUDE: ' + decoded[len(prefix):]).encode('utf-8')
    return decoded.encode('utf-8')


def main():
    arguments = sys.argv[1:]
    prefixes = []
    if len(arguments) >= 2 and arguments[0] == '--include-prefix-file':
        prefixes = Path(arguments[1]).read_text('utf-8').splitlines()
        arguments = arguments[2:]
    process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        for line in process.stdout:
            sys.stdout.buffer.write(normalize_line(line, prefixes))
        sys.stdout.buffer.flush()
        return process.wait()
    except KeyboardInterrupt:
        if process.poll() is None:
            process.terminate()
        process.wait()
        return 130
    finally:
        process.stdout.close()


if __name__ == '__main__':
    raise SystemExit(main())
