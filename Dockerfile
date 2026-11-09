# MiniVectorDB —— 从源码构建（单阶段）。
# 单阶段是刻意的选择：二进制和它的运行库（libstdc++）来自同一个文件系统，
# 不会出现"多阶段运行镜像缺 libstdc++.so.6 → 容器启动即退出"这类问题。
# 构建期跑一次 --selftest（建表/写入/向量 TopK/事务回滚/重开恢复），
# 二进制在镜像里跑不起来就直接让构建失败，而不是等到评测时才失败。
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    LC_ALL=C.UTF-8 \
    LANG=C.UTF-8

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ make \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./Makefile
COPY src ./src

RUN make -j"$(nproc)" \
 && ./minivectordb --selftest \
 && mkdir -p /app /data && chmod 777 /data \
 && install -m 0755 minivectordb /app/minivectordb \
 && rm -rf /src

WORKDIR /app
VOLUME ["/data"]

# stdin/stdout 与 MySQL 测试驱动交互，不监听端口
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
