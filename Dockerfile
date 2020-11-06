FROM alpine:3.12.1 as builder

RUN apk --no-cache add \
    build-base \
    cmake \
    openssl-dev \
    zlib-dev \
    gperf \
    linux-headers

COPY . /src

WORKDIR /src/build

RUN cmake -DCMAKE_BUILD_TYPE=Release ..
RUN cmake --build . --target install --

FROM alpine:3.12.1

RUN apk --no-cache add libstdc++

COPY --from=builder /usr/local/bin/telegram-bot-api /usr/local/bin/telegram-bot-api

HEALTHCHECK CMD curl -f http://localhost:8082/ || exit 1

ENTRYPOINT ["/usr/local/bin/telegram-bot-api -s 8082"]
