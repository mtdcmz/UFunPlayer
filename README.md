# UFunPlayer

Standalone Unity Web Player for Windows. Drag, drop, play – no browser needed.

## Usage

1. Place `UFunPlayer.exe` next to a `Runtime` folder (see Releases).
2. Launch the program.
3. Drag a `.unity3d` file onto the window, or use **File → Open**.
4. Press **F11** to toggle full‑screen.

The correct Unity runtime is selected automatically based on the bundle version.

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

## License

Licensed under the GNU General Public License v3.0.  
The full license text is in the [LICENSE](LICENSE) file.
