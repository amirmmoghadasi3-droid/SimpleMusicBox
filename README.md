# Music Player (Dear ImGui + DirectX 11 + miniaudio)

Requirements: Windows 10/11, Visual Studio 2022 with "Desktop development with C++".

1. Open `MusicPlayer.sln`, pick **x64** (Debug or Release), press F5.
2. Add music with Open / Ctrl+O, or drag files or folders onto the window (MP3, WAV, FLAC).
3. Window > Reset layout restores the default panel arrangement. Layout is saved to `imgui.ini`.

Vendored: Dear ImGui (docking branch, 1.93 WIP) in `third_party/imgui`, miniaudio in `third_party/miniaudio`.
