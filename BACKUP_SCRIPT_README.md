# Backup and Sync Scripts for deskHPSDR

This folder contains scripts to safely backup and synchronize your deskHPSDR source code between two directories.

## Scripts Overview

### 1. `backup_and_sync.sh` - Main Backup & Sync Script
Creates a timestamped backup of your current source code, then copies new files from the source directory.

**What it does:**
- Creates a timestamped backup of `/home/pi/src/deskhpsdr`
- Copies all files from `/home/pi/src2/deskhpsdr` to `/home/pi/src/deskhpsdr`
- Displays progress and summary

**Usage:**
```bash
chmod +x backup_and_sync.sh
./backup_and_sync.sh
```

**Output:**
- Backup location: `/home/pi/backups/deskhpsdr_backup_YYYYMMDD_HHMMSS/`
- Updated source: `/home/pi/src/deskhpsdr/`

### 2. `cleanup_old_backups.sh` - Cleanup Script
Removes backup directories older than a specified number of days to free up disk space.

**Usage:**
```bash
chmod +x cleanup_old_backups.sh

# Delete backups older than 30 days (default)
./cleanup_old_backups.sh

# Delete backups older than 7 days
./cleanup_old_backups.sh 7

# Delete backups older than 90 days
./cleanup_old_backups.sh 90
```

## Step-by-Step Instructions

### First Time Setup

1. **Copy scripts to your Pi:**
   ```bash
   # Download or copy the scripts to your home directory
   cd ~
   ```

2. **Make scripts executable:**
   ```bash
   chmod +x backup_and_sync.sh cleanup_old_backups.sh
   ```

3. **Verify directories exist:**
   ```bash
   ls -la /home/pi/src/deskhpsdr
   ls -la /home/pi/src2/deskhpsdr
   ```

### Running the Sync

1. **Execute the backup and sync:**
   ```bash
   ./backup_and_sync.sh
   ```

2. **Wait for completion** - You'll see a green checkmark when done

3. **Verify the copy was successful:**
   ```bash
   # Compare file counts
   find /home/pi/src/deskhpsdr -type f | wc -l
   find /home/pi/src2/deskhpsdr -type f | wc -l
   
   # Check git status
   cd /home/pi/src/deskhpsdr
   git status
   ```

### Managing Backups

**List all backups:**
```bash
ls -lhd /home/pi/backups/deskhpsdr_backup_*
```

**Free up disk space (keep last 30 days):**
```bash
./cleanup_old_backups.sh 30
```

**Free up disk space (keep last 7 days):**
```bash
./cleanup_old_backups.sh 7
```

**Restore from a backup:**
```bash
# List available backups
ls -d /home/pi/backups/deskhpsdr_backup_*/

# Restore from specific backup (example)
cp -r /home/pi/backups/deskhpsdr_backup_20250827_142530/* /home/pi/src/deskhpsdr/
```

## Directory Structure

```
/home/pi/
├── src/
│   └── deskhpsdr/          ← Current working copy
├── src2/
│   └── deskhpsdr/          ← New/updated files
└── backups/
    ├── deskhpsdr_backup_20250827_140000/
    ├── deskhpsdr_backup_20250826_120000/
    └── deskhpsdr_backup_20250825_100000/
```

## Safety Features

✓ **Timestamped backups** - Each backup has unique timestamp  
✓ **Automatic backup** - Current files backed up before sync  
✓ **Error handling** - Script exits on errors  
✓ **Verification** - File count displayed for confirmation  
✓ **Easy restore** - Backups available for recovery  

## Troubleshooting

### Backup directory already exists
- Multiple runs create separate timestamped backups
- Use `cleanup_old_backups.sh` to remove old ones

### Permission denied
```bash
# Make sure scripts are executable
chmod +x backup_and_sync.sh cleanup_old_backups.sh
```

### Directory not found
```bash
# Create missing directories if needed
mkdir -p /home/pi/backups
mkdir -p /home/pi/src/deskhpsdr
mkdir -p /home/pi/src2/deskhpsdr
```

### Low disk space
```bash
# Check available space
df -h /home/pi/

# Clean old backups to free space
./cleanup_old_backups.sh 7
```

## Advanced Usage

### Create a cron job for regular backups
```bash
# Edit crontab
crontab -e

# Add this line to run sync daily at 2 AM
0 2 * * * cd /home/pi && ./backup_and_sync.sh >> /home/pi/sync.log 2>&1
```

### Dry run (preview what will be copied)
```bash
# Check what will be copied without actually copying
diff -r /home/pi/src2/deskhpsdr /home/pi/src/deskhpsdr
```

### Check backup size
```bash
# Show size of backups
du -sh /home/pi/backups/deskhpsdr_backup_*/

# Total backup size
du -sh /home/pi/backups/
```

## Support

If you encounter issues:
1. Check that both source directories exist
2. Verify you have write permissions to both directories
3. Ensure you have sufficient disk space
4. Check script output for error messages
5. Review the backup directory contents to verify backup was created

---

**Note:** Always keep at least one recent backup before making critical changes to your source code.
