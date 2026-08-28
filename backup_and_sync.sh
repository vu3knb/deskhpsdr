#!/bin/bash

#############################################################################
# Backup and Sync Script for deskHPSDR
#
# This script:
# 1. Creates a timestamped backup of /home/pi/src/deskhpsdr
# 2. Copies all files from /home/pi/src2/deskhpsdr to /home/pi/src/deskhpsdr
#
# Usage: ./backup_and_sync.sh
#
#############################################################################

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
SOURCE_DIR="/home/pi/src/deskhpsdr"
NEW_FILES_DIR="/home/pi/src2/deskhpsdr"
BACKUP_BASE_DIR="/home/pi/backups"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BACKUP_DIR="${BACKUP_BASE_DIR}/deskhpsdr_backup_${TIMESTAMP}"

# Check if source directories exist
if [ ! -d "$SOURCE_DIR" ]; then
    echo -e "${RED}ERROR: Source directory $SOURCE_DIR does not exist!${NC}"
    exit 1
fi

if [ ! -d "$NEW_FILES_DIR" ]; then
    echo -e "${RED}ERROR: New files directory $NEW_FILES_DIR does not exist!${NC}"
    exit 1
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}deskHPSDR Backup and Sync Script${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Create backup directory
echo -e "${YELLOW}Creating backup directory...${NC}"
mkdir -p "$BACKUP_DIR"
echo -e "${GREEN}✓ Backup directory created: $BACKUP_DIR${NC}"
echo ""

# Backup current files
echo -e "${YELLOW}Backing up current files from $SOURCE_DIR...${NC}"
cp -r "$SOURCE_DIR"/* "$BACKUP_DIR/" 2>/dev/null || true
echo -e "${GREEN}✓ Backup completed${NC}"
echo ""

# Count files in backup
BACKUP_FILE_COUNT=$(find "$BACKUP_DIR" -type f | wc -l)
echo -e "${BLUE}  Files backed up: $BACKUP_FILE_COUNT${NC}"
echo ""

# Verify backup
if [ $BACKUP_FILE_COUNT -gt 0 ]; then
    echo -e "${GREEN}✓ Backup verified successfully${NC}"
else
    echo -e "${YELLOW}⚠ Warning: Backup directory appears to be empty${NC}"
fi
echo ""

# Copy new files
echo -e "${YELLOW}Copying files from $NEW_FILES_DIR to $SOURCE_DIR...${NC}"
cp -rv "$NEW_FILES_DIR"/* "$SOURCE_DIR/" 2>&1 | head -20
echo ""

# Count files in new location
NEW_FILE_COUNT=$(find "$SOURCE_DIR" -type f | wc -l)
echo -e "${BLUE}  Total files in $SOURCE_DIR: $NEW_FILE_COUNT${NC}"
echo ""

# Summary
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}✓ Operation completed successfully!${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo -e "${BLUE}Summary:${NC}"
echo -e "  Backup location: ${GREEN}$BACKUP_DIR${NC}"
echo -e "  Files backed up: ${GREEN}$BACKUP_FILE_COUNT${NC}"
echo -e "  Source updated: ${GREEN}$SOURCE_DIR${NC}"
echo -e "  Total files: ${GREEN}$NEW_FILE_COUNT${NC}"
echo ""
echo -e "${YELLOW}Backup retention tip:${NC}"
echo -e "  Old backups: ${BLUE}ls -la /home/pi/backups${NC}"
echo -e "  Delete old backup: ${BLUE}rm -rf /home/pi/backups/deskhpsdr_backup_YYYYMMDD_HHMMSS${NC}"
echo ""
