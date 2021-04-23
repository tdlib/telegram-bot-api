FROM alpine:latest AS build

RUN apk --no-cache --update add \
        alpine-sdk \
        gperf \
        cmake

COPY . /app
WORKDIR /app

RUN mkdir build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && cmake --build . --config Release

FROM scratch

COPY --from=build /etc/ssl/certs/ca-certificates.crt /etc/ssl/certs/ca-certificates.crt
COPY --from=build /app/build/telegram-bot-api

ENTRYPOINT ["/app/telegram-bot-api"]
