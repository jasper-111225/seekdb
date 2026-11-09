# MiniVectorDB —— 从源码构建（多阶段），运行镜像只带二进制与数据目录
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src ./src
COPY Makefile ./Makefile
RUN make -j"$(nproc)"

FROM ubuntu:24.04

WORKDIR /app
COPY --from=build /src/minivectordb /app/minivectordb
ENV LC_ALL=C.UTF-8 LANG=C.UTF-8
RUN mkdir -p /data
VOLUME ["/data"]

# stdin/stdout 与 MySQL 测试驱动交互，不监听端口
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
