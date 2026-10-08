halflife-cli release kit
========================

svencoop\
    Extract into your Sven Co-op root. This installs the plugin DLL
    (and PDB) into svencoop\metahook\plugins\, the config template and
    usermsg schemas into svencoop\metahook\configs\halflifecli\, and the
    gamedata catalog into svencoop\metahook\gamedata\halflifecli\.
    MetaHook must already be installed, and VGUI2Extension.dll must be
    present in svencoop\metahook\plugins\ and listed first in
    plugins.lst (console capture depends on it). install_plugin.bat
    from a source checkout does both; the archive never overwrites
    plugins.lst entries you already have — it ships no plugins.lst.

mcp\
    The MCP server and the Python tooling it needs at runtime (pure
    source, no build step; Python 3.11+). Run it with:

        uv run --script <absolute path>\mcp\halflife_mcp.py

    or with a plain environment:

        pip install "mcp>=2.2,<3" pillow pywin32
        python mcp\halflife_mcp.py

    Register it at user scope with your MCP client, e.g.:

        claude mcp add -s user halflife -- uv run --script <abs path>\mcp\halflife_mcp.py
        codex mcp add halflife -- uv run --script <abs path>\mcp\halflife_mcp.py

    The folder is location-independent; keeping it next to the game
    root is convenient but not required.

<archive>.sha256
    SHA-256 checksum of the 7z archive itself.

See https://github.com/DrAbcOfficial/halflife-cli for the full README.
