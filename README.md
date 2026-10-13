# 匿名意见收集系统

用 C17 编写、可容器化部署的匿名意见收集服务。任何人可以匿名提交意见，管理员可以通过受保护的接口查看、标记、备注和删除意见。系统在设计上不收集、不存储、不记录任何可以识别提交者身份的信息。

- HTTP 服务：libmicrohttpd
- 数据库：SQLite3（全部参数化 SQL）
- JSON：cJSON
- 标识：libuuid（UUID v4）
- 哈希与常量时间比较：OpenSSL libcrypto
- 并发：pthread + libmicrohttpd 线程池
- 构建：CMake
- 前端：原生 HTML/CSS/JS，黑白灰、无圆角、响应式

---

## 1. 隐私设计

| 要求 | 实现方式 |
| --- | --- |
| 不收集 IP / UA / Cookie / Referer / 账号 / 设备指纹 | 服务端只读取 `Authorization` 和 `Content-Type` 两个请求头，其余请求头既不解析也不记录；不读取 `X-Forwarded-*` |
| 数据库无身份字段 | `feedback` 表只有 id / content / category / status / admin_note / created_at_bucket / updated_at；启动时 `db_privacy_check()` 扫描列名，命中 `ip`、`user_agent`、`cookie`、`referer`、`session`、`device`、`fingerprint`、`token` 即拒绝启动 |
| 日志不含请求体 / 意见内容 / IP / Authorization | 请求日志只输出 `method path status`；libmicrohttpd 的内部日志通过 `MHD_OPTION_EXTERNAL_LOGGER` 全部丢弃（其消息可能带对端地址） |
| 不设置 Cookie | 不产生任何 `Set-Cookie`；前端不使用 `document.cookie` |
| 时间只存 UTC 小时桶 | `created_at_bucket` 形如 `2026-05-01T14:00:00Z`，由 `hour_bucket_utc()` 生成；`db_insert_feedback()` 在写入前用 `hour_bucket_valid()` 复核，任何精确到分钟/秒的时间都会被数据层拒绝 |
| 限流可用 IP 哈希，但只在内存、短 TTL、不落库、不写日志 | `ratelimit.c` 用 `HMAC-SHA256(salt, address)` 作为键，盐来自 `RATE_LIMIT_SALT`，未配置则每次启动用 `RAND_bytes` 随机生成；条目按窗口过期后被清零释放，从不写入数据库或日志 |
| 管理端认证 `Authorization: Bearer <ADMIN_TOKEN>`，只来自环境变量 | `config.c` 只从 `getenv("ADMIN_TOKEN")` 读取，无硬编码默认值 |
| ADMIN_TOKEN 未配置则启动失败 | `config_load()` 返回错误并退出码 1；少于 16 字符、含空白或明显弱口令同样拒绝启动 |
| 常量时间比较 | 先将两个令牌各自 SHA-256，再用 `CRYPTO_memcmp` 比较定长摘要，避免长度与提前返回泄漏 |
| 输出转义防 XSS | 服务端只输出 JSON，绝不渲染 HTML；前端所有服务端文本使用 `textContent` / `createTextNode` 写入（无任何 `innerHTML`），并配合 `Content-Security-Policy: default-src 'self'` 与 `X-Content-Type-Options: nosniff`；`html_escape_dup()` 作为经过测试的转义工具保留，供未来任何 HTML 渲染路径使用 |
| 不放宽 CORS | 不返回任何 `Access-Control-Allow-*` 头，浏览器按同源策略执行 |
| 请求体大小限制 64KB | `MAX_BODY_BYTES` 默认 65536，超出返回 `413 PAYLOAD_TOO_LARGE`，同时用 `MHD_OPTION_CONNECTION_MEMORY_LIMIT` 限制连接缓冲 |
| 管理端接口统一鉴权 | `route_admin()` 是 `/api/v1/admin/` 前缀的唯一入口，先限流再校验令牌，无法绕过 |
| 应用本身不记录访问者 IP | 对端地址只在 `ratelimit_allow()` 内即时哈希，从不落盘；建议由外层反向代理终止 HTTPS |

额外加固：

- 启动时 `setrlimit(RLIMIT_CORE, 0)` 关闭 core dump，避免内存中的意见内容被写盘。
- `umask(0077)`，数据库文件权限固定为 `0600`。
- 数据库以 WAL 模式打开，`busy_timeout=5000`。
- 静态文件只允许 `[A-Za-z0-9._/-]`，拒绝 `..`、绝对路径与符号链接（`O_NOFOLLOW`），文件大小上限 4MB。
- 响应统一带上 `CSP`、`X-Frame-Options: DENY`、`Referrer-Policy: no-referrer`、`Permissions-Policy`、`X-Content-Type-Options: nosniff`；API 与 HTML 均为 `Cache-Control: no-store`。

---

## 2. 目录结构

```
.
├── CMakeLists.txt
├── Dockerfile                 # 多阶段构建，构建阶段同时跑 ctest
├── docker-compose.yml
├── .env.example
├── .github/workflows/build.yml
├── migrations/
│   ├── 000_schema_migrations.sql
│   └── 001_init.sql           # feedback 表与索引
├── src/
│   ├── main.c                 # 启动、信号、--healthcheck
│   ├── config.c/.h            # 环境变量与校验
│   ├── util.c/.h              # 时间桶、UUID、UTF-8、常量时间比较、转义
│   ├── log.c/.h               # stderr / syslog 结构化日志
│   ├── ratelimit.c/.h         # 内存限流（IP 哈希）
│   ├── db.c/.h                # SQLite、迁移、CRUD
│   ├── http.c/.h              # 守护进程、路由、静态文件、安全响应头
│   └── handlers.c/.h          # 公共与管理 API
├── static/
│   ├── index.html             # 提交页 /
│   ├── admin.html             # 管理页 /admin
│   └── assets/{styles.css,app.js,admin.js}
└── tests/
    ├── test.h                 # 轻量断言宏
    ├── test_util.c
    ├── test_db.c
    ├── test_ratelimit.c
    └── integration_test.sh    # curl 端到端测试
```

---

## 3. 快速开始

### 3.1 依赖

Debian/Ubuntu：

```bash
sudo apt-get install -y build-essential cmake pkg-config \
  libmicrohttpd-dev libsqlite3-dev libcjson-dev uuid-dev libssl-dev \
  curl python3 sqlite3
```

### 3.2 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

可选开关：

- `-DENABLE_SANITIZERS=ON`：构建 AddressSanitizer + UndefinedBehaviorSanitizer 版本
- `-DWARNINGS_AS_ERRORS=OFF`：关闭 `-Werror`（默认开启）

### 3.3 运行

```bash
export ADMIN_TOKEN="$(openssl rand -hex 32)"
export DB_PATH=./data/feedback.db
export STATIC_DIR=./static
export MIGRATIONS_DIR=./migrations
./build/anonfeedback
```

打开 <http://127.0.0.1:8080/> 提交意见，<http://127.0.0.1:8080/admin> 进入管理端。

其它命令行参数：

- `--healthcheck`：向本地 `/api/v1/health` 发起一次探测，健康返回 0，否则返回 1（容器健康检查用，不需要 `ADMIN_TOKEN`）
- `--version`：打印版本

### 3.4 测试

```bash
cd build && ctest --output-on-failure
```

- `test_util`：时间桶、UUID、UTF-8 校验与计数、常量时间比较、枚举、参数解析、HTML 转义
- `test_db`：迁移幂等、文件权限、CRUD、筛选分页、CHECK 约束、SQL 注入无效化、隐私列扫描
- `test_ratelimit`：配额、IPv6、窗口过期、盐隔离、**断言内存中不存在原始 IP 字节**
- `integration`：真实进程 + curl 端到端（静态文件、路径穿越、字段校验、413、401/404/405/429、分页、备注、删除、响应头、日志与数据库隐私、启动拒绝）

---

## 4. 环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `ADMIN_TOKEN` | 无（必填） | 管理端令牌，≥16 字符，禁止空白与弱口令；未设置或不合格则启动失败 |
| `LISTEN_ADDRESS` | `0.0.0.0` | 监听地址 |
| `LISTEN_PORT` | `8080` | 监听端口 |
| `DB_PATH` | `./data/feedback.db` | SQLite 文件路径，父目录自动创建（权限 0700） |
| `STATIC_DIR` | `./static` | 静态资源目录 |
| `MIGRATIONS_DIR` | `./migrations` | 迁移文件目录 |
| `MAX_BODY_BYTES` | `65536` | 请求体上限，取值 1024–4194304 |
| `SUBMIT_RATE_MAX` | `10` | 每个 IP 哈希在窗口内的最大提交次数 |
| `SUBMIT_RATE_WINDOW_SECONDS` | `3600` | 提交限流窗口 |
| `ADMIN_RATE_MAX` | `600` | 每个 IP 哈希在窗口内的最大管理端请求次数 |
| `ADMIN_RATE_WINDOW_SECONDS` | `3600` | 管理端限流窗口 |
| `RATE_LIMIT_SALT` | 空 | 限流盐；留空则每次启动随机生成 |
| `LOG_LEVEL` | `info` | `error` / `warn` / `info` / `debug` |
| `LOG_TARGET` | `stderr` | `stderr` 或 `syslog` |
| `LOG_IDENT` | `anonfeedback` | syslog 标识 |
| `THREAD_POOL_SIZE` | `4` | libmicrohttpd 线程池大小 |
| `CONNECTION_LIMIT` | `128` | 最大并发连接数 |

---

## 5. API

统一前缀 `/api/v1`，请求与响应均为 JSON（`Content-Type: application/json`）。错误格式固定为：

```json
{ "error": { "code": "ERROR_CODE", "message": "可读错误信息" } }
```

`Feedback` 对象：

```json
{
  "id": "0f2a…-…",
  "content": "意见内容",
  "category": "bug",
  "status": "new",
  "admin_note": null,
  "created_at_bucket": "2026-05-01T14:00:00Z",
  "updated_at": null
}
```

### 5.1 公共接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/v1/health` | 健康检查，`{"status":"ok"}` |
| `GET` | `/api/v1/meta` | `{"categories":[…],"content_max_chars":5000}` |
| `POST` | `/api/v1/feedback` | 匿名提交，`201` 返回 `{id, status, category, created_at_bucket}`，`id` 即回执 ID |

提交校验：`content` 必填，去除首尾空白（含 U+3000）后 1–5000 个字符且必须是合法 UTF-8；`category` 可选，取值为 `bug` / `suggestion` / `complaint` / `other`（空串视为未填写）。

公共错误码：`INVALID_JSON`、`INVALID_CONTENT`、`INVALID_CATEGORY`、`UNSUPPORTED_MEDIA_TYPE`（415）、`PAYLOAD_TOO_LARGE`（413）、`RATE_LIMITED`（429）、`METHOD_NOT_ALLOWED`（405）。

### 5.2 管理接口

所有 `/api/v1/admin/*` 请求必须携带 `Authorization: Bearer <ADMIN_TOKEN>`，否则返回 `401 UNAUTHORIZED`。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/v1/admin/feedback?status=&limit=&offset=` | 分页列表，`limit` 默认 20、最大 100；`status` 可选 `new`/`read`/`archived` |
| `GET` | `/api/v1/admin/feedback/{id}` | 单条详情 |
| `PATCH` | `/api/v1/admin/feedback/{id}` | 修改 `status` 与/或 `admin_note`（`null` 表示清空备注） |
| `DELETE` | `/api/v1/admin/feedback/{id}` | 硬删除，成功返回 `204` |

列表响应：`{"items":[…],"total":N,"limit":L,"offset":O}`。

管理错误码：`UNAUTHORIZED`（401）、`INVALID_STATUS`、`INVALID_NOTE`（>2000 字符）、`EMPTY_UPDATE`、`INVALID_PAGINATION`、`NOT_FOUND`（404）。

### 5.3 页面路由

| 路径 | 内容 |
| --- | --- |
| `/`、`/index.html` | 提交页 |
| `/admin`、`/admin/`、`/admin.html` | 管理页 |
| `/assets/*` | CSS/JS 静态资源 |
| 其它 | 404 |

---

## 6. 数据模型与迁移

`migrations/001_init.sql`：

```sql
CREATE TABLE IF NOT EXISTS feedback (
    id                TEXT PRIMARY KEY NOT NULL,   -- UUID v4
    content           TEXT NOT NULL,
    category          TEXT NULL,                   -- 白名单
    status            TEXT NOT NULL DEFAULT 'new', -- new/read/archived
    admin_note        TEXT NULL,
    created_at_bucket TEXT NOT NULL,               -- UTC 小时桶
    updated_at        TEXT NULL,
    ...CHECK 约束...
);
CREATE INDEX IF NOT EXISTS idx_feedback_status_created
    ON feedback (status, created_at_bucket DESC);
```

迁移由程序启动时按文件名顺序执行，已应用版本记录在 `schema_migrations`，每个文件在独立事务中执行，无需额外工具。新增迁移只需放入 `MIGRATIONS_DIR`（命名如 `002_xxx.sql`）。

禁止向 `feedback` 添加任何提交者身份字段——启动时的 `db_privacy_check()` 与 CI 的 privacy-audit 任务会拦截。

---

## 7. 前端

- `/` 提交页：隐私说明、内容输入（`maxlength=5000` + 实时字数）、类别下拉、匿名提交、回执 ID（等宽字体 + 一键复制）。
- `/admin` 管理页：先输入令牌（仅保存在 `sessionStorage`，键 `anonfb_admin_token`），随后可筛选、分页、查看详情、改状态、写/清备注、两步确认删除。
- 黑白灰单色、1px 细线、零圆角、移动端与桌面端自适应；无任何第三方资源（离线可用），所有服务端文本以 `textContent` 写入。

---

## 8. 部署

### 8.1 Docker

```bash
docker build -t anonfeedback:local .
docker run -d --name anonfeedback \
  -e ADMIN_TOKEN="$(openssl rand -hex 32)" \
  -p 127.0.0.1:8080:8080 \
  -v feedback-data:/data \
  --read-only --tmpfs /tmp:size=16m \
  --cap-drop ALL --security-opt no-new-privileges \
  anonfeedback:local
```

镜像为多阶段构建：构建阶段编译并在镜像内执行完整 `ctest`；运行阶段仅保留运行库，以 UID/GID 10001 非 root 用户运行，内置 `HEALTHCHECK`（`anonfeedback --healthcheck`）。

### 8.2 docker-compose

```bash
cp .env.example .env
# 编辑 .env，填入 ADMIN_TOKEN
docker compose up -d --build
```

compose 默认只把端口发布到 `127.0.0.1`，并启用 `read_only`、`tmpfs`、`cap_drop: ALL`、`no-new-privileges` 与日志轮转。

### 8.3 HTTPS 与反向代理

应用只提供 HTTP，且不信任任何 `X-Forwarded-*` 头。请在外层（Nginx / Caddy / Traefik）终止 TLS，并注意：

- 代理层建议关闭访问日志，或至少不记录请求体；本应用不记录访问者 IP，代理层是否记录由你决定。
- 不要向应用传递访客身份头；应用会忽略它们。
- 如需对外暴露管理端，建议在代理层对 `/admin` 与 `/api/v1/admin/` 追加 IP 白名单或额外认证。

### 8.4 备份

只需备份 `/data/feedback.db`（WAL 模式下同时备份 `-wal`/`-shm`，或用 `sqlite3 feedback.db ".backup"`）。

---

## 9. CI/CD

`.github/workflows/build.yml`：

| 任务 | 内容 |
| --- | --- |
| `build`（matrix：amd64、arm64） | 在 `ubuntu-24.04` 与 `ubuntu-24.04-arm` 上安装依赖、Release 构建、执行 `ctest`，并以 `anonfeedback-linux-amd64` / `anonfeedback-linux-arm64` 上传二进制产物 |
| `sanitizers` | ASan + UBSan 下构建并跑全部测试 |
| `privacy-audit` | 校验迁移中无身份字段、前端无 Cookie/localStorage/innerHTML/外部资源、`ADMIN_TOKEN` 无硬编码默认值 |
| `docker` | QEMU + buildx 构建 `linux/amd64,linux/arm64` 多架构镜像；非 PR 事件登录 Docker Hub 并推送，同时上传镜像 digest 产物 |

推送镜像需要在仓库中配置两个 Secrets：

- `DOCKERHUB_USERNAME`
- `DOCKERHUB_TOKEN`

镜像名固定为 `<DOCKERHUB_USERNAME>/anonfeedback`，标签由 `docker/metadata-action` 生成（分支名、`v*` 语义化版本、默认分支的 `latest`）。PR 只构建不推送。

原生 arm64 构建使用 GitHub 托管的 `ubuntu-24.04-arm` runner（公开仓库默认可用；私有仓库需相应套餐支持，否则可把该矩阵项改为在 amd64 上交叉编译或仅保留 QEMU 多架构镜像构建）。

---

## 10. 设计假设

原始需求文档在「API 设计」一节给出统一前缀与错误格式后被截断，未列出具体端点。以下内容是按功能需求推导的保守设计，全部记录在此以便复核：

1. **端点命名**：公共提交为 `POST /api/v1/feedback`，管理端为 `/api/v1/admin/feedback[/{id}]`；列表用 `status`/`limit`/`offset` 查询参数。
2. **回执 ID 即 `feedback.id`**（UUID v4），不额外引入可被枚举的短码；不提供「凭回执查询」的公开接口，避免成为新的关联面。
3. **`category` 空串等于未填写**（存 `NULL`），非白名单值一律 400。
4. **`admin_note` 空串归一为 `NULL`**，`null` 显式清空；长度上限 2000 字符。
5. **删除为硬删除**（隐私优先，不留墓碑数据）；`204` 无响应体。
6. **`updated_at` 精确到秒**：它只反映管理员操作时间，与提交者无关联，压缩它会削弱审计能力。
7. **分页默认 20、最大 100**；`limit`/`offset` 越界直接 400，而不是静默夹取。
8. **限流按固定窗口计数**，提交与管理端各自独立配额；被限流的请求在解析请求体之前就被拒绝。
9. **`ADMIN_TOKEN` 额外要求 ≥16 字符且非弱口令**，这是对硬性要求 8 的保守加严；如需放宽请自行调整 `src/config.c`。
10. **静态资源挂在站点根路径**（`/assets/*`），不额外暴露 `/static/`。
11. **不提供 robots.txt**，避免对匿名提交页做任何额外约定。
12. 未实现用户可见的「按回执查询」、附件上传、多管理员账号体系等需求文档未提及的能力。

---

## 11. 已知限制

- 未内置 HTTPS，需要外部代理终止 TLS。
- 限流为单实例内存态；多副本部署时每个副本独立计数，且重启后清零（这是隐私与简洁性的取舍）。
- SQLite 适合单实例低并发场景；如需横向扩展需更换存储层。
- 管理端令牌只有一个，无角色划分与轮换机制。
- 无审计日志（有意为之：任何审计都会引入与提交者或管理行为的额外关联）。
