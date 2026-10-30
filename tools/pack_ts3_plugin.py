"""Build a TeamSpeak .ts3_plugin package (zip with package.ini + plugins/<dll>).

usage: pack_ts3_plugin.py <package.ini> <plugin.dll> <out.ts3_plugin>

Uses forward-slash entry names as the zip format requires (Windows PowerShell 5.1's
Compress-Archive writes backslashes, which TeamSpeak's installer may not accept)."""
import os, sys, zipfile

ini, dll, out = sys.argv[1:4]
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(ini, "package.ini")
    z.write(dll, "plugins/" + os.path.basename(dll))
print(f"packed {out}")
