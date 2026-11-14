# DXMT builtin dependency packaging

DXMT's `dxgi.dll` imports the private `winemetal.dll` in both PE architectures.
Wine normally needs a discoverable PE file before its selected DXMT runtime
redirect runs. Registering this dependency in the installed builtin directories
lets standard `wineboot` install it into prefixes without extra `WINEDLLPATH`.

The WineForge workspace packaging command
`build-wine-11.12-d3dmetal.sh dxmt-runtime-sync` invokes this helper from the
selected Wine source tree. It stages the two PE files from `lib/dxmt` into
matching `lib/wine` directories after validating both source architectures.
The host Unix library continues to load from `lib/dxmt/x86_64-unix`.

For an already installed complete runtime, register only these two dependencies:

```sh
python3 tools/dxmt/stage_builtin_dlls.py /path/to/WineForgeCore
python3 tools/dxmt/stage_builtin_dlls.py /path/to/WineForgeCore --check
```

Existing prefixes that lack the files need one standard prefix update using the
same Wine runtime as their game, after that prefix's programs have closed:

```sh
WINEPREFIX=/path/to/pfx /path/to/WineForgeCore/bin/wineboot -u
```

New prefixes obtain both files during normal initialization. The packaging helper
does not modify a prefix. It rejects invalid/missing source PE files, symlink
targets, unreadable files and replacements of mapped DLLs. Copies are verified
and unchanged files are left in place. The helper tests run from this source
root with `python3 -m unittest discover -s tools/dxmt -p 'test_*.py' -v`.
Set `WINEFORGE_WORKSPACE_BUILDER=/path/to/build-wine-11.12-d3dmetal.sh` to also
run both workspace integration checks. Without a workspace builder, those two
checks explicitly skip. The remaining nine helper checks still run.
