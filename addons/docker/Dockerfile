# SPDX-License-Identifier: BSD-3-Clause

FROM alpine:3.20 AS builder
RUN apk add --no-cache \
        build-base cmake pkgconf \
        lua5.4-dev yaml-dev argon2-dev ncurses-dev
WORKDIR /src
COPY . .
# Alpine keeps liblua in /usr/lib/lua5.4, where CMake's FindLua does not
# look (it finds the headers, then stops on "missing: LUA_LIBRARIES").
RUN cmake -B build \
        -DBUDYK_PLATFORM=linux \
        -DSTATIC_LINK=OFF \
        -DENABLE_LLM=OFF \
        -DLUA_LIBRARY=/usr/lib/lua5.4/liblua.so && \
    cmake --build build -j"$(nproc)" --target budyk && \
    strip build/src/budyk

FROM alpine:3.20
# libstdc++: budyk is C++ (without it: "Error loading shared library
# libstdc++.so.6" and nothing runs).
RUN apk add --no-cache libstdc++ lua5.4-libs yaml argon2-libs ncurses-libs \
 && addgroup -S -g 871 budyk \
 && adduser  -S -D -H -G budyk -u 871 -s /sbin/nologin budyk \
 && mkdir -p /var/db/budyk /etc/budyk \
 && chown -R budyk:budyk /var/db/budyk

COPY --from=builder /src/build/src/budyk /usr/local/bin/budyk
COPY config.example.yaml /etc/budyk/config.example.yaml
COPY rules/ /usr/share/budyk/rules/

USER budyk
VOLUME ["/var/db/budyk"]
EXPOSE 8080

ENTRYPOINT ["/usr/local/bin/budyk"]
CMD ["serve", "--config", "/etc/budyk/config.example.yaml"]
