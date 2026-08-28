#!/bin/bash

#############################################################################
# Cleanup Old Backups Script
#
# This script removes backups older than a specified number of days
# (default: 30 days)
#
# Usage: ./cleanup_old_backups.sh [days]
#        ./cleanup_old_backups.sh 30    # Delete backups older than 30 days
#
#############################################################################

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

BACKUP_DIR="/home/pi/backups"
DAYS_TO_KEEP=${1:-30}  # Default: keep backups from last 30 days

# Validate input
if ! [[ "$DAYS_TO_KEEP" =~ ^[0-9]+$ ]]; then
    echo -e "${RED}ERROR: Days must be a number${NC}"
    exit 1
fi

if [ ! -d "$BACKUP_DIR" ]; then
    echo -e "${YELLOW}Backup directory $BACKUP_DIR does not exist${NC}"
    exit 0
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Cleanup Old Backups${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Keeping backups from last ${YELLOW}$DAYS_TO_KEEP days${BLUE}...${NC}"
echo ""

# Find and delete old backups
DELETED_COUNT=0
while IFS= read -r backup_dir; do
    echo -e "${YELLOW}Deleting: $(basename "$backup_dir")${NC}"
    rm -rf "$backup_dir"
    ((DELETED_COUNT++))
done < <(find "$BACKUP_DIR" -maxdepth 1 -type d -name "deskhpsdr_backup_*" -mtime +$DAYS_TO_KEEP)

echo ""
echo -e "${BLUE}========================================${NC}"
if [ $DELETED_COUNT -gt 0 ]; then
    echo -e "${GREEN}✓ Deleted $DELETED_COUNT old backup(s)${NC}"
else
    echo -e "${GREEN}✓ No old backups to delete${NC}"
fi
echo -e "${BLUE}========================================${NC}"
echo ""

# Show remaining backups
echo -e "${BLUE}Remaining backups:${NC}"
ls -lhd "$BACKUP_DIR"/deskhpsdr_backup_* 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'

echo ""
