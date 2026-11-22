FROM ubuntu as BUILD

WORKDIR /app 

COPY . .

RUN apt update && apt install libssl-dev gperf git build-essential cmake zlib1g-dev ccache git -y 
RUN git submodule update --init --recursive
RUN mkdir build
RUN cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build . --target install

FROM ubuntu as DIST
WORKDIR /app
COPY --from=BUILD /app/build/telegram-bot-api  api
RUN chmod +x api
CMD ["./api"]
