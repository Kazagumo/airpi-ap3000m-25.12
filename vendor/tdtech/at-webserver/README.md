# at-webserver

OpenWrt 后端包，负责安装 MT5700 WebUI 所需的 C 版 WebSocket AT 服务和前端资源。

## 当前内容

- `src/main.c`: C 版后端源码
- `Makefile.native`: 本地/交叉编译使用的原生 Makefile
- `files/etc/init.d/at-webserver`: procd 服务脚本
- `files/etc/config/at-webserver`: UCI 默认配置
- `files/www/cgi-bin/at-ws-info`: WebSocket 地址发现接口
- `files/www/cgi-bin/at-log-clear`: 日志清空接口
- `files/www/5700/`: 独立 WebUI 前端

## 运行模型

- 服务名仍为 `at-webserver`
- 实际进程为 `/usr/bin/at-server-c`
- LuCI 和 `/5700/` 前端都读取 `at-webserver` 这份 UCI 配置

## 本地编译参考

如果只想在本地验证 C 源码，可以在本目录使用：

```sh
make -f Makefile.native
```

产物是 `at-server-c`。
