# syntax=docker/dockerfile:1

# Build and runtime use the same Ubuntu 24.04 release so the dynamic
# libstdc++/libgcc versions match exactly.  build.sh additionally links
# libstdc++/libgcc statically, so the runtime image only needs libc.
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
# The evaluation mounts /data; keep it writable even when the container runs
# with a non root user.
RUN mkdir -p /data && chmod 1777 /data
VOLUME ["/data"]
ENV MVDB_PROTOCOL=framed
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
STOPSIGNAL SIGTERM
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data", "--protocol", "framed"]
