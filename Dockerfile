# Multi-stage build: compile in a full toolchain image, ship a small runtime
# image with only the shared libraries the binaries need.
#
#   docker build -t tradebot .
#   docker run --rm tradebot tradebot-live --config /config/live.conf --check

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-13 cmake ninja-build libssl-dev zlib1g-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY third_party ./third_party
COPY src ./src
COPY tools ./tools
COPY tests ./tests
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 \
    && cmake --build build \
    && ./build/tests/tradebot_tests \
    && mkdir -p /out/bin \
    && cp build/tools/tradebot-* /out/bin/

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3t64 zlib1g ca-certificates tzdata tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system tradebot && useradd --system --gid tradebot --home /app --shell /usr/sbin/nologin tradebot \
    && mkdir -p /app /data /runs /config && chown tradebot:tradebot /app /data /runs \
    && ln -s /data /app/data && ln -s /runs /app/runs   # relative defaults (data/, runs/) land in the volumes
COPY --from=build /out/bin/ /usr/local/bin/
COPY configs/ /app/configs/
WORKDIR /app
USER tradebot
ENV TZ=UTC
# Data archive, run artifacts and configuration are volumes; secrets come
# from the environment (TRADEBOT_BINANCE_API_KEY / _SECRET), never files.
VOLUME ["/data", "/runs"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=60s --retries=3 \
    CMD ["/bin/sh", "-c", "tradebot-healthcheck /runs/${TRADEBOT_RUN:-paper-paper}/heartbeat --max-age 45s"]
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["tradebot-live", "--config", "/config/live.conf", "--check"]
