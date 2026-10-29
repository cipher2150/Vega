# syntax=docker/dockerfile:1
# Multi-stage build for linux/arm64 (Oracle Cloud Ampere A1). Build from the repo root:
#   docker build --platform linux/arm64 -f deploy/Dockerfile -t cbook:latest .

# ---- build stage: compiler, CMake, headers; runs the unit tests -------------------
FROM ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
ARG JOBS=2
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates libssl-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY src src
COPY tests tests
COPY bench bench
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCB_BUILD_BENCH=OFF \
 && cmake --build build -j"${JOBS}" \
 && ctest --test-dir build --output-on-failure

# ---- runtime stage: just the binary, the web page and shared libraries -------------
FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends libssl3t64 zlib1g ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --no-create-home cbook
COPY --from=build /src/build/cbook /opt/cbook/bin/cbook
COPY web /opt/cbook/web
COPY config /opt/cbook/config
WORKDIR /opt/cbook
USER cbook
EXPOSE 8080
ENTRYPOINT ["/opt/cbook/bin/cbook"]
CMD ["live", "--config", "/opt/cbook/config/production.json"]
