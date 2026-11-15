#!/bin/sh
# Daily backup of Forgejo (repositories, database, settings) into ./backups, keeping the last 14.
# Install: crontab -e, then the line:   30 4 * * * /root/forgejo/backup.sh >> /root/forgejo/backup.log 2>&1
# A copy off the server (Cloudflare R2, where the releases are) is added once rclone is set up: see README.md.
set -e
cd "$(dirname "$0")"
mkdir -p backups
stamp=$(date +%Y%m%d-%H%M)
docker exec -u git forgejo forgejo dump -c /data/gitea/conf/app.ini --file "/data/dump-$stamp.zip" --type zip
mv "forgejo/dump-$stamp.zip" "backups/forgejo-$stamp.zip"
ls -1t backups/forgejo-*.zip | tail -n +15 | xargs -r rm --
echo "backup: backups/forgejo-$stamp.zip"
