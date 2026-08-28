# Backup & Sync Changed Files Only

Simple script to backup and copy only the files that have changed.

## How to Use

### 1. Copy the script to your Pi:
```bash
# On your Pi
cd ~
```

### 2. Make it executable:
```bash
chmod +x backup_and_sync_changed.sh
```

### 3. Run it:
```bash
./backup_and_sync_changed.sh
```

## What It Does

1. **Compares files** between `/home/pi/src2/deskhpsdr` and `/home/pi/src/deskhpsdr`
2. **Backs up only changed files** to `/home/pi/backups/deskhpsdr_changed_YYYYMMDD_HHMMSS/`
3. **Copies only changed files** from src2 to src
4. **Shows summary** of what was copied

## Output Example

```
========================================
Backup & Sync Changed Files Only
========================================

Finding changed files...

  ● src/clock.c (modified)
  + src/clock.h (new)
  ● src/radio.c (modified)
  ● Makefile (modified)

========================================
Copying 4 changed file(s)...
========================================

  ✓ src/clock.c
  ✓ src/clock.h
  ✓ src/radio.c
  ✓ Makefile

========================================
✓ Complete!
========================================

Summary:
  Changed files: 4
  Backup location: /home/pi/backups/deskhpsdr_changed_20250827_143022
  Updated location: /home/pi/src/deskhpsdr
```

## Key Features

✓ **Only changed files** - Saves time and bandwidth  
✓ **Automatic backup** - Old versions saved before copying  
✓ **Shows what changed** - Visual indicators for modified (+) and new files (●)  
✓ **Fast** - Only processes changed files  

## Verify It Worked

```bash
# Check the copied files
ls -la /home/pi/src/deskhpsdr/src/clock.*

# Check backup was created
ls -la /home/pi/backups/deskhpsdr_changed_*/
```

## Need to Restore?

```bash
# Restore all changed files from backup
cp -r /home/pi/backups/deskhpsdr_changed_20250827_143022/* /home/pi/src/deskhpsdr/
```

## Delete Old Backups

```bash
# List all backups
ls -d /home/pi/backups/deskhpsdr_changed_*/

# Delete old backup
rm -rf /home/pi/backups/deskhpsdr_changed_20250827_143022
```
