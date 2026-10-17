# Build stage: full toolchain.
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY src src
COPY tests tests
RUN cmake --preset release && cmake --build --preset release -j"$(nproc)" --target kvstore

# Runtime stage: just the binary and the C++ runtime it links against.
FROM ubuntu:24.04
RUN useradd --system --create-home kvstore && mkdir /data && chown kvstore /data
COPY --from=build /src/build/release/kvstore /usr/local/bin/kvstore
USER kvstore
WORKDIR /data
VOLUME /data
EXPOSE 6380
ENTRYPOINT ["kvstore", "--bind", "0.0.0.0", "--aof-file", "/data/appendonly.aof"]
