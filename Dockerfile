# Ubuntu asosidagi imijni yuklash
FROM ubuntu:latest

# Kerakli paketlarni o‘rnatish
RUN apt-get update && apt-get install -y git g++ cmake make zlib1g-dev libssl-dev libgcrypt-dev

# Telegram Bot API’ni yuklash va yig‘ish (build)
RUN git clone https://github.com/tdlib/telegram-bot-api.git && \
    cd telegram-bot-api && \
    mkdir build && \
    cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    cmake --build . --parallel 4

# Portni ochish (8081 – HTTP port uchun)
EXPOSE 8081

# Local API serverni ishga tushirish
CMD ["telegram-bot-api/build/telegram-bot-api", "--local", "--http-port=8081"]
