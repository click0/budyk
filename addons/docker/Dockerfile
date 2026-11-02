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
# libstdc++.so.6" and nothing runs). curl: every alert channel posts
# through curl(1), and without it each alert failed (exit 127).
RUN apk add --no-cache libstdc++ lua5.4-libs yaml argon2-libs ncurses-libs curl \
 && addgroup -S -g 871 budyk \
 && adduser  -S -D -H -G budyk -u 871 -s /sbin/nologin budyk \
 && mkdir -p /var/db/budyk /etc/budyk \
 && chown -R budyk:budyk /var/db/budyk

COPY --from=builder /src/build/src/budyk /usr/local/bin/budyk
COPY addons/docker/budyk-health /usr/local/bin/budyk-health
COPY config.example.yaml /etc/budyk/config.example.yaml
COPY rules/ /usr/share/budyk/rules/
# The rules where the example config's rules.path looks: the image had
# none there, and budyk ran without rules.
COPY rules/examples.lua /usr/local/etc/budyk/rules.lua

USER budyk
VOLUME ["/var/db/budyk"]
# The example config listens on 127.0.0.1: the port is reachable with
# `--network host` (docker-compose.yml), or mount a config with
# `listen: 0.0.0.0` (plain HTTP: put a TLS proxy in front, see README).
EXPOSE 8080

# budyk as PID 1 reaps only its own children: run with `--init` (compose:
# `init: true`) so a helper an exec() command leaves behind is reaped.
# The check reads the address from the config the daemon runs with.
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s \
    CMD /usr/local/bin/budyk-health

ENTRYPOINT ["/usr/local/bin/budyk"]
CMD ["serve", "--config", "/etc/budyk/config.example.yaml"]
