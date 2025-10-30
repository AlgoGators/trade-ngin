DATE=$(date +%Y-%m-%d)
echo "$DATE"
cd /app
/app/build/bin/Release/live_trend "$DATE" --send-email