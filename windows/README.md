# EMB Thumbnails for Windows File Explorer

See previews of your **.EMB** (Wilcom) embroidery designs right in File Explorer,
the same way you see photo thumbnails. No Python and no Wilcom software needed.

## Install (one double-click)

1. Download **[`dist/EMB-Thumbnails-Setup.zip`](dist/EMB-Thumbnails-Setup.zip)** (click **Download raw file**, the ⤓ button), right-click it and choose **Extract All**.
   The bare [`.exe`](dist/EMB%20Thumbnails%20Setup.exe) is there too, but browsers often block .exe downloads.
2. Double-click it and click **OK**.
   - If Windows shows *"Windows protected your PC"*, click **More info**, then **Run anyway**.
     This appears because the program isn't code-signed.
3. Open a folder with .emb files and choose **View > Large icons** (or **Extra large icons**).

No administrator password is needed. It installs only for your own Windows account.

## Remove

**Settings > Apps > Installed apps > EMB Thumbnails > Uninstall.**
You can also double-click the setup file again and choose **No** (uninstall).

## Troubleshooting

| You see | Try this |
|---|---|
| Plain icons, no pictures | Use **View > Large icons**. Check that **File Explorer Options > View > "Always show icons, never thumbnails"** is **off**. |
| Old icons still show after installing | Close all File Explorer windows and open them again. If that doesn't help, run **Disk Cleanup** and tick **Thumbnails**. |
| One design shows no picture but others do | That file has no preview stored inside it. Opening and saving it once in Wilcom normally adds one. |
| Nothing shows and you have Wilcom EmbroideryStudio installed | Wilcom's own preview add-on takes priority, and you don't need this one. |

Requirements: Windows 10 or 11, 64-bit (Intel/AMD). Your EMB files are only read, never changed.

## How it works

An EMB file is a Microsoft "compound document" container. Wilcom keeps a small preview picture
inside it, usually in a part named `DESIGN_ICON`. This add-on is a standard Windows *thumbnail
provider*. When Explorer asks for a thumbnail, the add-on finds that picture (BMP, PNG, JPEG, GIF
or Windows metafile), enlarges it to the size Explorer wants, and hands it back. If a file has no
preview, Explorer shows the normal icon.

Files:
- `EmbThumbnailProvider.cpp`: the Explorer add-on (`EmbThumbnails.dll`)
- `emb_parse.h`: reads the EMB container and finds the picture (portable C++)
- `setup.cpp`: the double-click installer and uninstaller, with the DLL built into it
- `build.sh`: rebuilds everything on Linux with MinGW-w64 (`apt install g++-mingw-w64-x86-64`)

Silent install for IT use: `"EMB Thumbnails Setup.exe" /install /quiet`, and `/uninstall /quiet` to remove.
