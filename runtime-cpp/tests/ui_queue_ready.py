import json
from pathlib import Path
import subprocess
import sys


def main():
    runtime, assets, destination, edition = sys.argv[1:]
    with subprocess.Popen([runtime, '--worker', '--assets', assets], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0) as process:
        ready = process.stdout.readline()
        value = json.loads(ready)
        if value.get('type') != 'ready' or value.get('edition', {}).get('id') != edition:
            raise RuntimeError('Actual C++ edition/ready receipt differs from benchmark input')
        process.stdin.close()
        if process.wait(timeout=30):
            raise RuntimeError('Actual C++ ready fixture intake failed')
        Path(destination).write_bytes(ready)


if __name__ == '__main__':
    main()
