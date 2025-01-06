FROM ubuntu:24

RUN apt-get update && apt-get upgrade -y && \
    apt-get install -y make git zlib1g-dev libssl-dev gperf cmake clang-18 libc++-18-dev libc++abi-18-dev

RUN git clone --recursive https://github.com/tdlib/telegram-bot-api.git

WORKDIR /telegram-bot-api
RUN rm -rf build && mkdir build && cd build && \
    CXXFLAGS="-stdlib=libc++" CC=/usr/bin/clang-18 CXX=/usr/bin/clang++-18 \
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX:PATH=.. .. && \
    cmake --build . --target install

EXPOSE 8080
CMD ["/telegram-bot-api/bin/telegram-bot-api", "--http-port", "8080", "--http-ip-address", "0.0.0.0"]
