# diskrescue

Recover data from an external disk with damaged partitions or bad sectors.
It is one Python 3 file with no dependencies, and it **never writes to the damaged disk**.

## First: stop and do no harm

- **Do not** click "Initialize", "Format", or "Repair" when Windows or macOS offers it.
- **Do not** run `chkdsk`, `fsck`, or a partition "fix" directly on the damaged disk.
- If the disk clicks, grinds, or keeps disconnecting, image it **once** (step 2) and stop using it.
  For data that matters, a professional lab is the safe choice.
- You need free space on a **different, healthy** drive that is at least as large as the damaged disk.

## Steps (Linux, or macOS with `sudo`)

```bash
# 0. Find the disk: Linux `lsblk`, macOS `diskutil list`. Use the whole disk (/dev/sdb), not /dev/sdb1.

# 1. Look at what's left of the partition tables (MBR, primary and backup GPT)
sudo python3 diskrescue.py info /dev/sdb

# 2. Clone the disk to an image. Bad areas are skipped first and retried later.
#    Press Ctrl-C to pause, then run the same command again to resume.
sudo python3 diskrescue.py image /dev/sdb /mnt/healthy/disk.img
#    Tip: add --no-retry to grab the easy data first, then rerun without it.

# 3. Find partitions from their filesystem signatures (NTFS, FAT, exFAT, ext2/3/4, HFS+, APFS),
#    even when the partition table is gone
python3 diskrescue.py scan /mnt/healthy/disk.img

# 4. Mount a found partition READ-ONLY and copy your files off (keeps names and folders)
sudo mkdir -p /mnt/rescue
sudo mount -o ro,loop,offset=<OFFSET> /mnt/healthy/disk.img /mnt/rescue
#    Or pull the partition out on its own:
python3 diskrescue.py extract /mnt/healthy/disk.img part1.img --offset <OFFSET> --length <SIZE>

# 5. Last resort, if a partition won't mount: carve files out by their signatures
python3 diskrescue.py carve /mnt/healthy/disk.img /mnt/healthy/carved/
```

Carving recovers JPG, PNG, GIF, PDF, DOCX, XLSX, PPTX, ODT, ZIP, MP4, MOV, HEIC, M4A, 3GP, WAV, AVI, and WEBP.
You can limit it with `--types jpg,mp4,zip`.
Carved files lose their original names and folders, and fragmented files may come back incomplete.

## When something goes wrong

| Symptom | What to do |
|---|---|
| `info` says the primary GPT is damaged but the backup is OK | The partitions in the backup table are valid. Use their offsets with `mount` or `extract`. On a copy (an image or a new disk), `gdisk` can rebuild the primary table from the backup. |
| `scan` notes "may be the BACKUP boot sector" for NTFS | The start of the volume is damaged. Mount at the alternate offset it prints. If that doesn't work, run `ntfsfix` on an **extracted copy** of the partition. |
| A partition mounts but files are missing or the mount fails | Run `fsck` or `chkdsk` on an **extracted copy** (`part1.img`), never on the original disk. |
| Imaging is extremely slow or hangs on bad areas | Pause with Ctrl-C and use [GNU ddrescue](https://www.gnu.org/software/ddrescue/), which has read timeouts and handles dying drives better. Then run steps 3–5 on its image. |
| You want more file types or a guided interface | Run [TestDisk and PhotoRec](https://www.cgsecurity.org/) **on the image**. TestDisk rebuilds partition tables, and PhotoRec carves hundreds of formats. |

On Windows, the safest route is to boot a Linux live USB and follow the steps above.

---

# embthumbs

See the thumbnails of Wilcom **.EMB** embroidery files, without Wilcom installed.
One Python 3 file, no dependencies, works on Windows, macOS and Linux. It only reads your files.

EMB files are Microsoft "compound document" containers. Wilcom puts a small preview picture
inside (usually in a part called `DESIGN_ICON`). `embthumbs` pulls that picture out.

```bash
# Build a thumbnail gallery for a whole folder (subfolders included), then open emb_gallery/index.html
python3 embthumbs.py gallery "C:\Designs" -o emb_gallery

# Save the preview picture(s) of one file
python3 embthumbs.py extract design.emb -o previews

# Troubleshoot one file: list what's inside it
python3 embthumbs.py info design.emb
```

In the gallery you can filter by name, resize the thumbnails, and click one to open the EMB file.

If a file shows "no preview", run `info` on it and share the output. Some EMB versions may store
the preview in a format this tool doesn't decode yet.
