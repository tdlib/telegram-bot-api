FROM ubuntu:latest

ENV TZ=Europe/Kiev

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    cmake \
    g++ \
    build-essential \
    gperf \
    openssl \
    libssl-dev \
    zlib1g-dev \
    git \
    && rm -rf /var/lib/apt/lists/*


RUN git clone --recursive https://github.com/tdlib/telegram-bot-api.git && \
    cd telegram-bot-api/ && \
    mkdir build && \
    cd build

WORKDIR telegram-bot-api/build

RUN cmake .. -DCMAKE_BUILD_TYPE=Release \
    && cmake --build . --target install

CMD ["telegram-bot-api","--api-id=$API_ID", "--api-hash=$API_HASH", "--local"]

EXPOSE 8081