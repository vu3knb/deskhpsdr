#!/bin/bash

#############################################################################
# Quick Sync Script (without backup)
#
# Fast sync of files from source to destination without creating backups
# Use this for regular quick updates when you're confident about changes
#
# Usage: ./sync_only.sh
#
# WARNING: This does NOT create a backup. Use backup_and_sync.sh if you
#          want to preserve a copy of the current files first!
#
#############################################################################

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Configuration
SOURCE_DIR="/home/pi/src/deskhpsdr"
NEW_FILES_DIR="/home/pi/src2/deskhpsdr"

# Validation
if [ ! -d "$SOURCE_DIR" ]; then
    echo -e "${RED}ERROR: Source directory $SOURCE_DIR does not exist!${NC}"
    exit 1
fi

if [ ! -d "$NEW_FILES_DIR" ]; then
    echo -e "${RED}ERROR: New files directory $NEW_FILES_DIR does not exist!${NC}"
    exit 1
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}deskHPSDR Quick Sync${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
echo -e "${YELLOW}⚠ WARNING: No backup will be created!${NC}"
echo -e "Source: ${BLUE}$NEW_FILES_DIR${NC}"
echo -e "Target: ${BLUE}$SOURCE_DIR${NC}"
echo ""

# Confirm before proceeding
read -p "$(echo -e ${YELLOW})Continue? (y/N)$(echo -e ${NC}) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${YELLOW}✗ Cancelled${NC}"
    exit 0
fi

echo ""
echo -e "${YELLOW}Syncing files...${NC}"
cp -rv "$NEW_FILES_DIR"/* "$SOURCE_DIR/" 2>&1 | tail -10
echo ""

# Show summary
FILE_COUNT=$(find "$SOURCE_DIR" -type f | wc -l)
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}✓ Sync completed!${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "Total files in target: ${GREEN}$FILE_COUNT${NC}"
echo ""
