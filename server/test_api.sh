#!/bin/bash
# Тест API kill-switch сервера

URL=${1:-"http://localhost:10000"}
SECRET=${2:-"CHANGE_ME_SUPER_SECRET_12345"}

echo "Testing $URL with secret $SECRET"
echo ""

echo "1. GET /api/status"
curl -s $URL/api/status | jq . || curl -s $URL/api/status
echo -e "\n"

echo "2. GET /api/check?hwid=test123&version=1.0.0"
curl -s "$URL/api/check?hwid=test123&version=1.0.0" | jq . || curl -s "$URL/api/check?hwid=test123&version=1.0.0"
echo -e "\n"

echo "3. POST disable"
curl -s -X POST $URL/api/admin/status \
  -H "Content-Type: application/json" \
  -d "{\"secret\":\"$SECRET\",\"enabled\":false,\"message\":\"Disabled via test script\"}" | jq . || true
echo -e "\n"

echo "4. GET status after disable (should be enabled=false)"
curl -s $URL/api/status | jq . || curl -s $URL/api/status
echo -e "\n"

echo "5. POST enable back"
curl -s -X POST $URL/api/admin/status \
  -H "Content-Type: application/json" \
  -d "{\"secret\":\"$SECRET\",\"enabled\":true,\"message\":\"Enabled via test script\"}" | jq . || true
echo -e "\n"

echo "6. Final status (should be enabled=true)"
curl -s $URL/api/status | jq . || curl -s $URL/api/status
echo -e "\nDone!"
