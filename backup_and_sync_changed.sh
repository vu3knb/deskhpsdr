#!/bin/bash

#############################################################################
# Backup and Sync Changed Files Only
#
# Backs up and copies only the files that are different between:
# /home/pi/src2/deskhpsdr (new files)
# /home/pi/src/deskhpsdr (current files)
#
# Usage: ./backup_and_sync_changed.sh
#
#############################################################################

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
SOURCE_DIR="/home/pi/src/deskhpsdr"
NEW_FILES_DIR="/home/pi/src2/deskhpsdr"
BACKUP_BASE_DIR="/home/pi/backups"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BACKUP_DIR="${BACKUP_BASE_DIR}/deskhpsdr_changed_${TIMESTAMP}"

# Validate directories
if [ ! -d "$SOURCE_DIR" ]; then
    echo -e "${RED}ERROR: $SOURCE_DIR does not exist!${NC}"
    exit 1
fi

if [ ! -d "$NEW_FILES_DIR" ]; then
    echo -e "${RED}ERROR: $NEW_FILES_DIR does not exist!${NC}"
    exit 1
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Backup & Sync Changed Files Only${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Create backup directory
mkdir -p "$BACKUP_DIR"
echo -e "${YELLOW}Finding changed files...${NC}"
echo ""

# Find and backup changed files only
CHANGED_COUNT=0
while IFS= read -r file; do
    # Get relative path
    rel_path="${file#$NEW_FILES_DIR/}"

    if [ -f "$SOURCE_DIR/$rel_path" ]; then
        # File exists in both - check if different
        if ! cmp -s "$file" "$SOURCE_DIR/$rel_path"; then
            # Files are different - back up the old one
            mkdir -p "$(dirname "$BACKUP_DIR/$rel_path")"
            cp "$SOURCE_DIR/$rel_path" "$BACKUP_DIR/$rel_path"
            ((CHANGED_COUNT++))
            echo -e "  ${YELLOW}●${NC} $rel_path (modified)"
        fi
    else
        # File is new in source
        mkdir -p "$(dirname "$BACKUP_DIR/$rel_path")"
        touch "$BACKUP_DIR/$rel_path"  # Create placeholder
        ((CHANGED_COUNT++))
        echo -e "  ${GREEN}+${NC} $rel_path (new)"
    fi
done < <(find "$NEW_FILES_DIR" -type f)

echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${YELLOW}Copying $CHANGED_COUNT changed file(s)...${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Copy changed files
while IFS= read -r file; do
    rel_path="${file#$NEW_FILES_DIR/}"
    mkdir -p "$(dirname "$SOURCE_DIR/$rel_path")"
    cp "$file" "$SOURCE_DIR/$rel_path"
    echo -e "  ${GREEN}✓${NC} $rel_path"
done < <(find "$NEW_FILES_DIR" -type f)

echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}✓ Complete!${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo -e "${BLUE}Summary:${NC}"
echo -e "  Changed files: ${GREEN}$CHANGED_COUNT${NC}"
echo -e "  Backup location: ${GREEN}$BACKUP_DIR${NC}"
echo -e "  Updated location: ${GREEN}$SOURCE_DIR${NC}"
echo ""
