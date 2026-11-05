import hashlib
import io
import os
import urllib.request
import zipfile

TAG = 'r2117'
ZIP = 'https://github.com/vgmstream/vgmstream/releases/download/%s/vgmstream-win64.zip' % TAG
ZIP_SHA = '6c4a8a3813864fefed081bbd337dbc0ad93bf88e0b92f5db98d7ab258b22dc6c'
OUT = r'C:\Users\Frame\Downloads\openworld_work\vgmstream'


def main():
    exe = os.path.join(OUT, 'vgmstream-cli.exe')
    if os.path.isfile(exe):
        print('already present:', exe)
        return 0
    data = urllib.request.urlopen(ZIP, timeout=120).read()
    if hashlib.sha256(data).hexdigest() != ZIP_SHA:
        raise SystemExit('%s: checksum mismatch' % ZIP)
    os.makedirs(OUT, exist_ok=True)
    zipfile.ZipFile(io.BytesIO(data)).extractall(OUT)
    print('extracted to', OUT)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
