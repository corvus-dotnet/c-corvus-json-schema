FROM alpine:3.22 AS build
RUN apk add --no-cache cmake gcc musl-dev make curl jq
# Optionally pin the C library's version so `build-all` can rebuild historical versions. An empty value (the normal
# build) takes the latest release (tagged capi-v<version> in Corvus.JsonSchema).
ARG IMPLEMENTATION_VERSION
RUN version="${IMPLEMENTATION_VERSION}" \
 && if [ -z "$version" ]; then \
      version=$(curl -sSfL "https://api.github.com/repos/corvus-dotnet/Corvus.JsonSchema/releases?per_page=100" \
        | jq -r '[.[] | select(.tag_name | startswith("capi-v")) | select((.draft or .prerelease) | not)][0].tag_name | ltrimstr("capi-v")'); \
    fi \
 && name="corvus-json-schema-$version-$(uname -m)-unknown-linux-musl" \
 && curl -sSfL "https://github.com/corvus-dotnet/Corvus.JsonSchema/releases/download/capi-v$version/$name.tar.gz" | tar -xz -C /opt \
 && mv "/opt/$name" /opt/corvus-json-schema
COPY CMakeLists.txt bowtie_corvus_json_schema.c /src/
RUN cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/corvus-json-schema \
 && cmake --build /build --parallel 4

FROM alpine:3.22
RUN apk add --no-cache libgcc
COPY --from=build /build/bowtie_corvus_json_schema /usr/local/bin/
CMD ["bowtie_corvus_json_schema"]
