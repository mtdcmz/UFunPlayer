# UFunPlayer

Standalone Unity Web Player for Windows. Drag, drop, play – no browser needed.

## Usage

1. Place `UFunPlayer.exe` next to a `Runtime` folder (see Releases).
2. Launch the program.
3. Drag a `.unity3d` file onto the window, or use **File → Open**.
4. Press **F11** to toggle full‑screen.

The correct Unity runtime is selected automatically based on the bundle version.

## Tools Integration

UFunPlayer can launch external tools (e.g., decompilers, asset extractors, or custom utilities) directly from its **Tools** menu.

- Place any `.exe` files you want to use inside a `Tools` folder next to `UFunPlayer.exe`.
- By default the Tools menu is hidden. To enable it, go to **Tools → Enable Tools** and confirm the warning dialog.
- Once enabled, the menu will show a **Refresh** button and list all `.exe` files found in the `Tools` folder.
- Click any tool name to launch it. The tool’s working directory is set to the folder containing the `.exe`.
- Tools are loaded on startup if previously enabled; you can refresh the list at any time.

> **Note:** Enabling the Tools feature allows arbitrary executables to be run. Only place trusted tools in the `Tools` folder.

## Save Data (PlayerPrefs) Warning

Unity Web Player saves game data (PlayerPrefs) by encoding the **full path** of
the `.unity3d` file into the save file name.  

If that path is very long, or contains many non‑ASCII characters (e.g. Chinese),
the resulting save path can exceed Windows’ 260‑character `MAX_PATH` limit.  
When that happens, **saves are silently lost** — the file is simply never written.

### How to keep saves working
- Move the `.unity3d` file to a short, **all‑English** folder, for example:  
  `C:\Games\game.unity3d`
- Keep both the folder names and the file name as short as possible.
- Avoid deep nested directories.

UFunPlayer will warn you on open if it detects that the save path is too long.

## Clearing User Data

The **Help → Clear User Data** menu item resets your recent‑file history and disables the Tools feature (you will need to re‑enable it via the menu). This is useful for privacy or troubleshooting.

## License

Licensed under the GNU General Public License v3.0.  
The full license text is in the [LICENSE](LICENSE) file.
