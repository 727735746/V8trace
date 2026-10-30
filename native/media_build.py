import hashlib
import argparse
import json
from pathlib import Path
import re


REQUIRED = dict(proprietary_codecs='true', ffmpeg_branding='"Chrome"', media_use_openh264='true',
    media_use_ffmpeg='true', media_use_libvpx='true')


def configure(data):
    text = data.decode('utf-8-sig')
    for key, value in REQUIRED.items():
        pattern = r'(?m)^[ \t]*' + re.escape(key) + r'[ \t]*=[^\r\n]*'
        matches = re.findall(pattern, text)
        if len(matches) > 1:
            raise ValueError('Duplicate media build argument: ' + key)
        line = key + ' = ' + value
        if matches:
            text = re.sub(pattern, lambda unused: line, text)
        else:
            text = text.rstrip() + '\n' + line + '\n'
    return text.replace('\r\n', '\n').encode('utf-8')


def verify(file):
    data = Path(file).read_bytes()
    text = data.decode('utf-8-sig')
    for key, expected in REQUIRED.items():
        values = re.findall(r'(?m)^[ \t]*' + re.escape(key) + r'[ \t]*=[ \t]*([^\r\n]*)', text)
        if [value.split('#', 1)[0].strip() for value in values] != [expected]:
            raise ValueError('Media-enabled Chromium requires ' + key + ' = ' + expected)
    return dict(protocol='v8trace/media-build/1', proprietaryCodecs=True, ffmpegBranding='Chrome',
        openH264=True, ffmpeg=True, libvpx=True, gnArgsSha256=hashlib.sha256(data).hexdigest(),
        runtimePlaybackValidated=False, runtimeEncodingValidated=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--verify', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(verify(args.verify)))
