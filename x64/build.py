"""Build one WarcraftXL x64 provider in an x64 Visual Studio developer shell."""
import argparse
from pathlib import Path
import json
import shutil
import subprocess
import os

def run(*args,cwd=None):
    subprocess.run(list(map(str,args)),cwd=cwd,check=True)

def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--sdk",required=True,type=Path)
    cli.add_argument("--build",required=True,type=Path,help="new, empty output directory")
    cli.add_argument("--webview2",type=Path)
    cli.add_argument("--mpv",type=Path)
    cli.add_argument("--prepare-only",action="store_true")
    opt=cli.parse_args()
    here=Path(__file__).resolve().parent
    repo=here.parent
    build=opt.build.resolve()
    if build.exists() and any(build.iterdir()):
        raise SystemExit("Choose an empty build directory.")
    build.mkdir(parents=True,exist_ok=True)
    worker=build/"worker-source"
    shutil.copytree(repo/"host",worker)
    patch=here/"worker.patch"
    for line in patch.read_text(encoding="utf-8").splitlines():
        if line.startswith("--- a/"):
            target=worker/line[6:]
            target.write_bytes(target.read_text(encoding="utf-8-sig").encode("utf-8"))
    # The host is an x86 extension's helper in the ordinary fork build.
    # Apply IPC and architecture changes to this private x64 build tree.
    run("git","init","-q",cwd=worker)
    run("git","apply",patch,cwd=worker)
    if opt.prepare_only: return
    manifest=json.loads((here/"mod.json").read_text())
    is_plex=manifest["id"]=="vixal.azerothplex"
    dependency=opt.mpv if is_plex else opt.webview2
    if not dependency: cli.error("Supply --mpv for Plex or --webview2 for Cinema.")
    options=["-G","Ninja","-DCMAKE_BUILD_TYPE=Release","-DCMAKE_TRY_COMPILE_CONFIGURATION=Release"]
    run("cmake","-S",here,"-B",build/"mod",*options,"-DWXL_SDK="+str(opt.sdk.resolve()))
    run("cmake","--build",build/"mod","--parallel","4")
    key="MPV_ROOT" if is_plex else "WEBVIEW2_ROOT"
    run("cmake","-S",worker,"-B",build/"worker",*options,"-DBUILD_TESTING=OFF","-D"+key+"="+str(dependency.resolve()))
    run("cmake","--build",build/"worker","--target","wxl-video-host","--parallel","4")
    dest=build/"package/Mods"/manifest["id"]
    binary=Path(manifest["binaries"]["win-x64"])
    (dest/binary.parent).mkdir(parents=True)
    media=dest/"Media";media.mkdir()
    shutil.copy2(build/"mod"/binary.name,dest/binary)
    shutil.copy2(build/"worker/wxl-video-host.exe",media)
    shutil.copy2(here/"mod.json",dest)
    shutil.copytree(here/"WXLFilmClub",build/"package/Interface/AddOns/WXLFilmClub")
    for name in ("LICENSE","THIRD_PARTY_NOTICES.md","MODIFICATIONS.md"): shutil.copy2(repo/name,media)
    if is_plex:
        shutil.copy2(opt.mpv/"libmpv-2.dll",media)
    else:
        shutil.copy2(repo/"player.html",media)
    print("Built",dest)

if __name__=="__main__": main()

