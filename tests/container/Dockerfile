FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build g++-14 git python3 curl \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
COPY . .
ARG LB_GIT_REVISION=unknown
RUN cmake --preset release -DCMAKE_CXX_COMPILER=g++-14 -DCMAKE_C_COMPILER=gcc-14 \
      -DLB_GIT_REVISION=${LB_GIT_REVISION} \
    && cmake --build --preset release --parallel 2 \
    && useradd --uid 10001 --no-create-home tester \
    && chown -R 10001:10001 /work/build
USER 10001:10001
CMD ["sh", "scripts/container_chaos.sh"]
