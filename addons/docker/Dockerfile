# SPDX-License-Identifier: BSD-3-Clause
FROM alpine:3.20 AS builder
RUN apk add --no-cache build-base cmake lua5.4-dev yaml-dev
WORKDIR /src
COPY . .
RUN cmake -B build -DSTATIC_LINK=ON -DENABLE_LLM=OFF && cmake --build build -j$(nproc)

FROM scratch
COPY --from=builder /src/build/src/budyk /budyk
COPY config.example.yaml /etc/budyk/config.yaml
COPY rules/ /usr/share/budyk/rules/
ENTRYPOINT ["/budyk"]
CMD ["serve", "--config", "/etc/budyk/config.yaml"]
EXPOSE 8080
