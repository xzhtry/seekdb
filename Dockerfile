# syntax=docker/dockerfile:1

# Build and runtime use the same Ubuntu 24.04 release so the dynamic
# libstdc++/libgcc versions match exactly.
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY src ./src
COPY build.sh ./build.sh
RUN chmod +x build.sh && ./build.sh

FROM ubuntu:24.04
WORKDIR /app
COPY --from=build /src/build/minivectordb /app/minivectordb
RUN mkdir -p /data
VOLUME ["/data"]
ENV MVDB_PROTOCOL=framed
STOPSIGNAL SIGTERM
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data", "--protocol", "framed"]
