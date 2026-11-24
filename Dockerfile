FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build g++-14 git \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY apps ./apps
COPY scripts/run_proxy.py ./scripts/
COPY examples ./examples
COPY docs/security.md docs/protocol-support.md docs/release-usage.md ./docs/
COPY monitoring/README.md ./monitoring/
ARG LB_GIT_REVISION=unknown
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_C_COMPILER=gcc-14 -DBUILD_TESTING=OFF -DLB_GIT_REVISION=${LB_GIT_REVISION} \
    && cmake --build build --parallel 2

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libstdc++6 curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin balancer
COPY --from=build /src/build/load_balancer /src/build/test_backend /usr/local/bin/
USER 10001:10001
ENTRYPOINT ["/usr/local/bin/load_balancer"]
