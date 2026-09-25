# Лицензионный сервер — ноль зависимостей, npm install не нужен.
FROM node:20-alpine

WORKDIR /app
COPY package.json server.js ./

ENV PORT=8080
EXPOSE 8080

# SECRET и ADMIN_SECRET приезжают через `fly secrets set` — в образ не печём!
CMD ["node", "server.js"]
