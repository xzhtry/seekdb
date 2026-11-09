# syntax=docker/dockerfile:1

# Build stage: a pinned g++ image, no network access is needed at run time.
FROM gcc:13 AS build
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
