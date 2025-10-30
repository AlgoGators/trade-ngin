DATE=$(date +%Y-%m-%d)
echo "$DATE"
./build/bin/Release/live_trend "$DATE" --send-email