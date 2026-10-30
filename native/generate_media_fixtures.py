import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def generate(ffmpeg, output):
    output.mkdir(parents=True, exist_ok=False)
    common = [str(ffmpeg), '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
        'testsrc2=size=160x96:rate=15', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '2']
    variants = [('fixture.mp4', ['-c:v', 'libx264', '-profile:v', 'baseline', '-pix_fmt', 'yuv420p',
        '-c:a', 'aac', '-b:a', '64k', '-movflags', '+faststart']),
        ('fixture.webm', ['-c:v', 'libvpx-vp9', '-b:v', '150k', '-c:a', 'libopus', '-b:a', '64k'])]
    files = []
    for name, flags in variants:
        command = [*common, *flags, str(output / name)]
        result = subprocess.run(command, capture_output=True, timeout=120)
        (output / (name + '.encoder.log')).write_bytes(result.stderr)
        if result.returncode:
            raise RuntimeError('Synthetic media generation failed: ' + name)
        data = (output / name).read_bytes()
        files.append(dict(path=name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), command=command))
    manifest = dict(protocol='v8trace/synthetic-media-fixtures/1', source='generated color pattern and sine wave', files=files)
    (output / 'fixtures.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--ffmpeg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(generate(args.ffmpeg.resolve(strict=True), args.output.resolve())))
