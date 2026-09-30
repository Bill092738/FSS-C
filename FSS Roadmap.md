# FSS 技术路线（高保真模型）

Sep 30, 2026 · @Bill

## 1. 定位与约束

结论：FSS 高保真模型 = 单机、单文件数据库、单个 C 可执行文件对外提供全部 API 与实时推送；LLM 冷启动管线作为离线批处理独立存在，只通过“导入 SQLite”与后端交接。

**目标（要做到“高保真”的部分）**

- 业务规则完整：空间 schema、多维筛选、拥挤度众包与加权、积分与徽章、审核、预测、Study With Me、热力图、券、B 端聚合报表，都有真实可跑的实现而不是 mock。
- 数据真实：至少一所学校（默认 OSU）由 Phase 0 管线产出 100+ 个 Spot，走完“爬取 → 抽取 → 归一 → 入库”全链路。
- 实时性真实：拥挤度上报后，订阅了该 Spot 的客户端在同一事件循环内收到推送（WebSocket）。
- 可复制性真实：换一所学校只改一份 campus 配置文件即可重跑管线。

**非目标**

- 不做水平扩展、多机部署、在线数据库、edge 部署、CDN。
- 不追求生产级的可用性、备份、合规审计；但隐私相关的设计（脱敏、k-匿名）要按真实规则实现，因为它影响 B 端变现逻辑。
- 不做原生 App：前端用 Web（可装为 PWA），浏览器定位 API 足以验证地理围栏逻辑。

**技术选型总览**

| 层 | 选型 | 理由 |
| --- | --- | --- |
| HTTP / WebSocket / SSE / Pub/Sub | [facil.io cstl 0.8.x](https://github.com/facil-io/cstl)，单头文件 `fio-stl.h` | 约束指定；同一个 reactor 同时承载 REST 与实时推送 |
| 数据库 | SQLite 3（WAL 模式）+ FTS5 + R\*Tree | 约束指定；FTS5 做文本检索，R\*Tree 做空间范围查询 |
| JSON | cstl 的 FIOBJ（`fiobj_json_parse` / `fiobj2json`） | 与框架同源，免引第三方库 |
| 密码与令牌 | cstl 的 `fio_argon2` 与 `fio_blake2b_hmac` | 同上，库内已有实现 |
| Phase 0 管线 | Python 3.12 + httpx + selectolax + Claude API（结构化输出） | 爬虫和 LLM 调用用脚本语言迭代最快 |
| 前端 | Vite + TypeScript + Preact + MapLibre GL（OSM 瓦片） | 轻量、无商业地图 key |
| 构建 | Makefile + clang/gcc，`-fsanitize=address,undefined` 调试构建 | 纯 C 项目最直接的方式 |
| 测试 | C 单元测试（自写最小 harness）+ Python pytest 做 API 黑盒测试 + k6 压测 | 黑盒测试不受后端语言限制 |

已验证：在本环境用 gcc 13.3 编译 `fio-stl.h`（开启 `FIO_HTTP`、`FIO_PUBSUB`、`FIO_FIOBJ`）并链接 SQLite 3.45.1，`fio_http_listen` + `fio_http_route` + `fiobj2json` 的最小服务可正常响应请求。注意：系统存在 OpenSSL 头文件时 cstl 会自动启用 OpenSSL TLS 后端，链接时需要加 `-lssl -lcrypto`。

## 2. 核心业务逻辑提炼

商业计划去掉修辞后，可实现的内核是六条规则链：空间有标准描述 → 按当下任务筛选 → 用户在现场上报状态 → 按可信度加权聚合 → 贡献换积分 → 聚合数据脱敏后给商家与校方。其余内容（“数字孪生”“心流”“一周内光速扩张”）是愈景，不转化为需求。

**领域实体**

| 实体 | 含义 | 关键属性 |
| --- | --- | --- |
| Campus | 一所学校，多校区隔离的单位 | slug、时区、边界框、学期日历 |
| Building | 教学楼/图书馆 | 名称与别名、质心坐标、地理围栏多边形、开放时间 |
| Spot | 可学习的最小空间（一层、一间教室、一个角落） | 标准化 schema 四大块：坐标、基建、规则、氛围 |
| Claim | 对 Spot 某属性的一条主张，带来源与置信度 | 属性键、值、来源（llm/user/official）、证据引文、票数 |
| Report | 一次现场状态上报 | 拥挤度（0 绿 / 1 黄 / 2 红）、事件类型、是否在围栏内、权重 |
| CheckIn | 一次入座到离开的会话 | 开始/结束时间、是否验证在场 |
| User | 用户 | 专业、课程、语言、信誉分（reputation）、积分（karma） |
| Session（Study With Me） | 限时的组队学习邀请 | Spot、课程、语言、人数上限、过期时间 |
| Merchant / Offer / Coupon | 商家、优惠模板、发放给用户的券 | 触发条件、库存、核销码 |

**六条核心规则**

1. **Spot 属性是“被投票的主张”，不是字段。** LLM 抽取、用户修正、官方数据都写成 Claim；Spot 上的“当前值”是 Claim 的加权结果的物化视图。这样 AI 冷启动数据和众包数据共用一套流程，也天然支持“顶/踩淘汰过时信息”。
2. **检索 = 硬条件过滤 + 软条件排序。** “必须有白板”“允许吃外卖”是过滤；“极度安静”是对噪音等级排序；距离与实时拥挤度也参与排序。
3. **拥挤度是会衰减的加权估计。** 每条 Report 的权重 = 信誉分 × 围栏系数 × 时间衰减；没有新上报时回落到预测值并标记“预测”。
4. **在场证明决定话语权。** 服务端用客户端上报的坐标与精度判断是否在建筑围栏内；围栏外上报仍被接收但权重降为 0.2。
5. **贡献换积分，积分与信誉分分离。** 积分可消费（换券）；信誉分不可消费，只随贡献被社区确认或否定而涨跌，并反过来影响权重。
6. **对外数据只出聚合。** 热力图、B 端报表、“工科生在哪聚集”均按 k-匿名（默认 k=5）过滤，少于 k 个独立用户的格子不输出。

飞轮在技术上的对应：Phase 0 写入 Claim（来源 llm）→ 用户检索并到场 → Report 与 CheckIn 写入 → 投票修正 Claim、积分入账 → 聚合表喂给预测、热力图、券触发与 B 端报表。

## 3. 系统总体架构

结论：在线部分只有一个 C 进程和一个 SQLite 文件；LLM 只出现在离线的 Phase 0 管线里，两者之间唯一的接口是数据库表结构。

&#91;embedded content: 总体架构 · 在线一个 C 进程，离线一条 Python 管线\]

读图：浏览器的 REST 请求与 WebSocket 消息由 reactor 线程解析后派到 HTTP 线程池（高亮，业务代码全在这里）；线程池写库并在事务提交后 publish，由 reactor 推给订阅者。定时器在 reactor 上触发，把涉及数据库的工作转投到单线程作业队列，作业队列也通过 publish 把变化推出去。

一次拥挤度上报的完整数据流：

1. 前端 `POST /api/v1/spots/42/reports`，带 level 与坐标精度。
2. reactor 解析请求，把 `on_http` 排进 HTTP 线程池。
3. 处理线程校验会话与频率限制，计算围栏系数与权重（纯计算，不在事务内）。
4. `BEGIN IMMEDIATE`：插入 report，重算 `occupancy_live`，写 `karma_ledger` 并更新 `user.karma`，`COMMIT`。
5. 颜色或 basis 有变化则 `fio_pubsub_publish` 到 `spot:42` 与 `bldg:{id}`；积分变化 publish 到 `user:{id}`。
6. 返回 201 与新的拥挤度状态；订阅者收到推送。

## 4. C 后端：facil.io cstl 0.8.x

结论：单进程（`fio_io_start(0)`）+ 一个 HTTP 异步线程池（`fio_io_async_s`，4 线程），每个线程持有自己的 SQLite 连接；REST 走 `fio_http_route` 前缀路由，实时推送走 WebSocket + 内置 Pub/Sub。以下 API 均对照 cstl master（提交 `24a5701`，2026-08-21）的头文件核实，并实际编译运行过。

### 4.1 引入方式与模块开关

cstl 是单头文件库：把 `fio-stl.h` 放进 `vendor/`，写一个项目头 `fss_fio.h`，内含 `#define FIO_EXTERN` 加各模块宏再 include；所有 `.c` 都引这个头，只有 `fio_impl.c` 在引入前额外定义 `FIO_EXTERN_COMPLETE`，由它独家产生实现（见 `250 fiobj.md` 与 `001 compiler attributes.md`）。这样每个文件不用重复编译 10 万行实现。`FIO_HTTP` 会在 `000 dependencies.h` 中自动打开 `FIO_JSON`、`FIO_MULTIPART`、`FIO_URL_ENCODED`、`FIO_PUBSUB`、`FIO_IPC`、`FIO_IO`。

```c
/* src/fss_fio.h — 所有 .c 共用 */
#define FIO_EXTERN
#define FIO_LOG
#define FIO_CLI
#define FIO_FIOBJ     /* JSON 与动态类型 */
#define FIO_HTTP      /* 自动带入 IO / IPC / PUBSUB / JSON / MULTIPART */
#define FIO_ARGON2    /* 密码哈希 */
#define FIO_BLAKE2    /* 令牌 HMAC */
#include "fio-stl.h"

/* src/fio_impl.c — 唯一产生实现的单元 */
#define FIO_EXTERN_COMPLETE
#include "fss_fio.h"
```

编译链接：`cc -std=gnu11 -O2 ... -lsqlite3 -lssl -lcrypto -lpthread -lm`。只要系统有 OpenSSL 头文件，cstl 就会编译 OpenSSL 后端，不加 `-lssl -lcrypto` 会报 `BIO_meth_free` 等未定义符号（实测）。本项目本地跑 HTTP 即可，TLS 留给前面的开发代理或直接不用。

### 4.2 进程与线程模型

- reactor 是每进程单线程的；`fio_io_start(workers)` 中 `workers > 0` 时 fork 出子进程，master 负责 IPC 与崩溃重启。
- `fio_http_settings_s.queue` 指向一个 `fio_io_async_s` 时，`on_http`、`on_message` 等用户回调在该线程池里执行（文档：“User callbacks are scheduled through the selected HTTP task queue”）。这是把阻塞的 SQLite 调用移出 IO 线程的正规手段。
- 线程池每个线程启动时触发 `FIO_CALL_ON_WORKER_THREAD_START`（`102 queue.h` 中的 `fio_state_callback_force` 调用）。在这里打开一个 `__thread sqlite3 *` 连接，在 `FIO_CALL_ON_WORKER_THREAD_END` 关闭。

```c
static fio_io_async_s HTTP_Q = FIO_IO_ASYN_INIT;
static __thread sqlite3 *tl_db;

static void db_thread_open(void *arg)  { tl_db = fss_db_open((const char *)arg); }
static void db_thread_close(void *arg) { sqlite3_close_v2(tl_db); tl_db = NULL; (void)arg; }

int main(int argc, char const *argv[]) {
  fio_state_callback_add(FIO_CALL_ON_WORKER_THREAD_START, db_thread_open, (void *)"data/fss.db");
  fio_state_callback_add(FIO_CALL_ON_WORKER_THREAD_END, db_thread_close, NULL);
  fio_io_async_attach(&HTTP_Q, 4);
  fio_http_listener_s *l = fio_http_listen("0.0.0.0:8080",
      .on_http = route_fallback, .queue = &HTTP_Q,
      .on_authenticate_websocket = ws_auth, .on_open = ws_on_open,
      .on_message = ws_on_message, .on_close = ws_on_close,
      .public_folder = FIO_STR_INFO1("web/dist"),
      .max_body_size = 8 << 20, .ws_max_msg_size = 16 << 10,
      .compress_dynamic = 1, .log = 1);
  routes_register(l);
  jobs_register();          /* fio_io_run_every 定时任务 */
  fio_io_start(0);
}
```

**为什么默认不开多 worker 进程：** SQLite 在任一时刻只有一个写者，多进程只会把锁竞争从线程间搬到进程间；并且 fork 前打开的连接不能在子进程继续使用。需要演示集群时，把连接建立点改到 `FIO_CALL_ON_START`（每个 worker 进程启动时）即可，Pub/Sub 会自动经 IPC 跨进程分发，业务代码不变。

### 4.3 路由与路径参数

- `fio_http_route(listener, "/api/v1/spots", .on_http = spots_handler)` 是**最佳前缀匹配**；在回调中 `fio_http_opath(h)` 是原始路径，`fio_http_path(h)` 是去掉前缀后的剩余部分。
- `fio_http_resource_action(h)` 按方法与剩余路径给出 INDEX / SHOW / CREATE / UPDATE / DELETE 等。**实测它只看第一段**：`GET /api/v1/spots/42/reports` 返回 SHOW，`POST` 同路径返回 UPDATE。因此嵌套资源用两种办法之一：给子资源单独注册更长的前缀，或在回调内自己切分 `fio_http_path` 的段。
- 项目内封装一个小路由表：`{method, pattern, handler}`，pattern 形如 `/:id/reports`，在前缀回调内匹配并把 `:id` 解析为 `int64_t` 传给 handler。这是纯 C 后端里唯一需要自己写的“框架”代码，约 150 行。
- 静态文件：`public_folder` 只对 GET/HEAD 生效，未命中则落到 `on_http`，正好用来做 SPA 回退（返回 `index.html`）。注意 `max_age` 不被子路由继承。

### 4.4 请求与响应

- 读：`fio_http_method`、`fio_http_query`、`fio_http_request_header(h, name, index)`、`fio_http_cookie`、`fio_http_body_read`。请求体用 `fiobj_json_parse(fio_http_body_read(h, len), NULL)` 解析；也可用 `fio_http_body_parse` 自动识别 JSON / urlencoded / multipart（照片上传用后者）。
- 写：`fio_http_status_set`、`fio_http_response_header_set`、`fio_http_write(h, .buf, .len, .copy / .dealloc, .finish = 1)`。错误统一走自己的 `api_error(h, status, code, msg)`，输出 JSON。
- 字符串拼接用 `fio_bstr_write2(NULL, FIO_STRING_WRITE_STR2(...), ...)`，响应时把 `fio_bstr_free` 作为 `.dealloc` 交出所有权。
- JSON 输出：简单对象用 FIOBJ 构造后 `fiobj2json`；列表类大响应直接用 SQLite 的 `json_group_array(json_object(...))` 在 SQL 里生成整段 JSON，C 端原样写出。这是减少 C 代码量最有效的一招。

### 4.5 WebSocket 与 Pub/Sub

- 升级鉴权：`on_authenticate_websocket` 返回非 0 即拒绝；在这里校验 cookie 中的会话令牌，并用 `fio_http_udata2_set` 把 user\_id 挂到连接上。
- 订阅：客户端发 `{"op":"sub","ch":"spot:42"}`，服务端调用 `fio_http_subscribe(h, .channel = FIO_BUF_INFO2(ch, len), .on_message = FIO_HTTP_WEBSOCKET_SUBSCRIBE_DIRECT_TEXT)`。订阅归属于连接，连接关闭时自动取消；同一连接对同一频道只保留一个订阅。
- 发布：拥挤度聚合更新后 `fio_pubsub_publish(.channel = ..., .message = ...)`，消息体是小 JSON。
- 频道命名：`spot:{id}`（单点状态）、`bldg:{id}`（楼内汇总）、`campus:{slug}:live`（地图视野内的客户端）、`swm:{session_id}`（Study With Me 房间）、`user:{id}`（个人通知：积分、券）。也可用 `.is_pattern = 1` 做通配订阅，但默认不用，避免广播风暴。
- 重连补发：`fio_pubsub_history_attach(fio_pubsub_history_cache(size_limit), priority)` 挂上内存历史缓存后，订阅时传 `.replay_since = last_ts_ms` 即可补发断线期间的消息。
- 并发安全：文档明确 `fio_http_websocket_write` 对同一连接必须串行化。规则：业务代码**从不**直接写别人的 WebSocket，只 `publish`；只在该连接自己的 `on_message` 回调里写回复。

### 4.6 定时任务

`fio_io_run_every(.fn, .udata1, .every = ms, .repetitions = -1)`，`fn` 返回非 0 即停止。注意：头文件注释写的是 `on_stop`，但结构体 `fio_timer_schedule_args_s` 的实际字段名是 `on_finish`，以结构体为准。定时回调跑在 IO 线程上，所以涉及 SQLite 的任务在回调里只做 `fio_io_async(&JOB_Q, .fn = job, ...)` 转投到专门的 1 线程作业队列。

| 任务 | 间隔 | 内容 |
| --- | --- | --- |
| live\_decay | 60 s | 重算活跃 Spot 的拥挤度估计，变化则 publish |
| checkin\_timeout | 5 min | 关闭超过 6 小时未结束的 CheckIn |
| hourly\_rollup | 1 h | 把 Report / CheckIn 聚合进 `occupancy_hourly` |
| forecast\_rebuild | 24 h | 重建预测表 `forecast_slot` |
| swm\_expire | 60 s | 关闭过期的 Study With Me 房间并 publish |
| wal\_checkpoint | 10 min | `PRAGMA wal_checkpoint(PASSIVE)` |

### 4.7 内存与调试纪律

- `fio_http_s` 只在回调期间有效；需要延后写响应时先 `fio_http_dup`，写完 `fio_http_free`。
- 一切 `sqlite3_stmt` 用预编译缓存（每线程一个 `FIO_MAP`，键为 SQL 字符串地址），用完 `sqlite3_reset`，不反复 prepare。
- 调试构建开 `-fsanitize=address,undefined` 与 `-DDEBUG`，让 cstl 的泄漏计数器在退出时报告未释放对象。

## 5. SQLite 数据层

结论：一个 `data/fss.db` 文件，WAL 模式；Spot 的可筛选属性由 Claim 物化为定宽列加一个 64 位特征位图，文本检索用 FTS5，范围查询用 R\*Tree，列表响应直接在 SQL 里拼 JSON。

### 5.1 构建与连接设置

- 把 SQLite amalgamation（`sqlite3.c`）放进 `vendor/`，固定版本并用以下编译宏编译：`-DSQLITE_THREADSAFE=2`（多线程模式，每个连接只归一个线程，正好对应 4.2 的线程模型）、`-DSQLITE_ENABLE_FTS5`、`-DSQLITE_ENABLE_RTREE`、`-DSQLITE_DQS=0`、`-DSQLITE_DEFAULT_FOREIGN_KEYS=1`。系统自带的 libsqlite3 也能用（本环境为 3.45.1），但功能开关因发行版而异。
- 每个连接打开后执行：`PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-20000;`。
- 写事务一律用 `BEGIN IMMEDIATE`，让写锁在事务开始时就拿到，避免读转写升级时的 `SQLITE_BUSY` 死锁。写事务要短：不在事务内做哈希、网络或大 JSON 构造。
- 迁移：`migrations/NNNN_name.sql` 顺序执行，版本记在 `PRAGMA user_version`，启动时由主线程在 `fio_io_start` 之前跑完。

### 5.2 核心表

```sql
-- 校园与建筑
CREATE TABLE campus (
  id INTEGER PRIMARY KEY, slug TEXT UNIQUE NOT NULL, name TEXT NOT NULL,
  tz TEXT NOT NULL,                       -- IANA 时区，如 America/New_York
  bbox_json TEXT NOT NULL,                -- [minLon,minLat,maxLon,maxLat]
  calendar_json TEXT                      -- 学期、考试周、假期区间
);
CREATE TABLE building (
  id INTEGER PRIMARY KEY, campus_id INTEGER NOT NULL REFERENCES campus(id),
  name TEXT NOT NULL, aliases_json TEXT,  -- 别名，用于 LLM 实体对齐
  lat REAL NOT NULL, lon REAL NOT NULL,
  fence_json TEXT NOT NULL,               -- 多边形顶点 [[lon,lat],...]
  hours_json TEXT
);
CREATE VIRTUAL TABLE building_rtree USING rtree(id, min_lon, max_lon, min_lat, max_lat);

-- Spot：物化视图列由 claim 聚合重算，不被业务直接写
CREATE TABLE spot (
  id INTEGER PRIMARY KEY, building_id INTEGER NOT NULL REFERENCES building(id),
  name TEXT NOT NULL, floor TEXT, lat REAL, lon REAL,
  features INTEGER NOT NULL DEFAULT 0,   -- 64 位布尔特征位图，位定义见 attr_def
  noise INTEGER,                          -- 0 极静 .. 4 嘈杂
  outlets INTEGER,                        -- 0 无 .. 3 极多
  temp INTEGER,                           -- -2 很冷 .. 2 很热
  capacity INTEGER,                       -- 估计座位数
  vibe TEXT,                              -- deep_work / collab / casual
  summary TEXT, pros_json TEXT, cons_json TEXT,
  quality REAL NOT NULL DEFAULT 0,        -- 综合可信度 0..1
  status TEXT NOT NULL DEFAULT 'active',  -- active / hidden / merged
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL
);
CREATE INDEX spot_bldg ON spot(building_id) WHERE status = 'active';
CREATE VIRTUAL TABLE spot_fts USING fts5(name, summary, tags, content='', tokenize='unicode61');

-- 属性注册表：标准化 schema 的唯一事实来源
CREATE TABLE attr_def (
  key TEXT PRIMARY KEY,                   -- whiteboard, food_allowed, noise, ...
  kind TEXT NOT NULL,                     -- flag / ordinal / enum / text
  bit INTEGER UNIQUE,                     -- flag 类对应 spot.features 的位
  domain_json TEXT, label_zh TEXT, label_en TEXT
);

-- 主张与投票
CREATE TABLE claim (
  id INTEGER PRIMARY KEY, spot_id INTEGER NOT NULL REFERENCES spot(id),
  attr TEXT NOT NULL REFERENCES attr_def(key), value TEXT NOT NULL,
  source TEXT NOT NULL,                   -- llm / official / user
  user_id INTEGER REFERENCES user(id), source_doc_id INTEGER REFERENCES source_doc(id),
  evidence TEXT,                          -- 原文引语
  prior REAL NOT NULL,                    -- 初始置信度
  up INTEGER NOT NULL DEFAULT 0, down INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);
CREATE INDEX claim_spot_attr ON claim(spot_id, attr);
CREATE TABLE claim_vote (
  claim_id INTEGER REFERENCES claim(id), user_id INTEGER REFERENCES user(id),
  v INTEGER NOT NULL CHECK (v IN (-1, 1)), weight REAL NOT NULL, at INTEGER NOT NULL,
  PRIMARY KEY (claim_id, user_id)
) WITHOUT ROWID;

-- 实时状态
CREATE TABLE report (
  id INTEGER PRIMARY KEY, spot_id INTEGER NOT NULL REFERENCES spot(id),
  user_id INTEGER NOT NULL REFERENCES user(id),
  level INTEGER CHECK (level IN (0,1,2)),  -- 绿/黄/红，事件类可为 NULL
  event TEXT,                              -- outlet_broken / closed_event / ...
  in_fence INTEGER NOT NULL, accuracy_m REAL, weight REAL NOT NULL,
  at INTEGER NOT NULL
);
CREATE INDEX report_spot_at ON report(spot_id, at DESC);
CREATE TABLE checkin (
  id INTEGER PRIMARY KEY, spot_id INTEGER NOT NULL, user_id INTEGER NOT NULL,
  start_at INTEGER NOT NULL, end_at INTEGER, verified INTEGER NOT NULL
);
CREATE UNIQUE INDEX checkin_open ON checkin(user_id) WHERE end_at IS NULL;
CREATE TABLE occupancy_live (
  spot_id INTEGER PRIMARY KEY, est REAL NOT NULL,  -- 0..2 连续估计
  conf REAL NOT NULL, basis TEXT NOT NULL,          -- reports / forecast
  updated_at INTEGER NOT NULL
);
CREATE TABLE occupancy_hourly (
  spot_id INTEGER, hour_ts INTEGER,        -- 整点 UTC 秒
  n_reports INTEGER, n_users INTEGER, mean_level REAL, n_checkins INTEGER,
  PRIMARY KEY (spot_id, hour_ts)
) WITHOUT ROWID;
CREATE TABLE forecast_slot (
  spot_id INTEGER, day_type TEXT,          -- normal / exam / break
  dow INTEGER, slot INTEGER,               -- 星期几、半小时槽 0..47（校园本地时间）
  level REAL, n INTEGER,
  PRIMARY KEY (spot_id, day_type, dow, slot)
) WITHOUT ROWID;

-- 用户、积分、徽章
CREATE TABLE user (
  id INTEGER PRIMARY KEY, email TEXT UNIQUE NOT NULL, pw_hash BLOB NOT NULL, pw_salt BLOB NOT NULL,
  display_name TEXT, campus_id INTEGER, major TEXT, langs_json TEXT, courses_json TEXT,
  reputation REAL NOT NULL DEFAULT 1.0,    -- 0.1 .. 3.0
  karma INTEGER NOT NULL DEFAULT 0,        -- 余额缓存，以 karma_ledger 为准
  created_at INTEGER NOT NULL
);
CREATE TABLE auth_session (token_hash BLOB PRIMARY KEY, user_id INTEGER NOT NULL, expires_at INTEGER NOT NULL) WITHOUT ROWID;
CREATE TABLE karma_ledger (
  id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, delta INTEGER NOT NULL,
  reason TEXT NOT NULL, ref_type TEXT, ref_id INTEGER, at INTEGER NOT NULL,
  UNIQUE (user_id, reason, ref_type, ref_id)   -- 幂等：同一贡献只记一次
);
CREATE TABLE badge (key TEXT PRIMARY KEY, name TEXT, rule_json TEXT);
CREATE TABLE user_badge (user_id INTEGER, badge_key TEXT, at INTEGER, PRIMARY KEY (user_id, badge_key)) WITHOUT ROWID;
CREATE TABLE photo (
  id INTEGER PRIMARY KEY, spot_id INTEGER, user_id INTEGER, path TEXT, sha256 BLOB UNIQUE,
  up INTEGER DEFAULT 0, down INTEGER DEFAULT 0, status TEXT DEFAULT 'visible', at INTEGER
);

-- Phase 0 溯源
CREATE TABLE source_doc (
  id INTEGER PRIMARY KEY, campus_id INTEGER, url TEXT UNIQUE, kind TEXT,  -- official / reddit / maps / blog
  fetched_at INTEGER, content_sha256 BLOB, license_note TEXT
);
```

Phase 2 与变现相关的表（`swm_session`、`swm_member`、`merchant`、`offer`、`coupon`、`agg_cell`）在第 8 节定义。

### 5.3 关键查询形态

多维检索是一条语句：硬条件变成位运算与数值比较，软条件变成 `ORDER BY` 表达式，结果在 SQL 里直接拼成 JSON。

```sql
SELECT json_group_array(json_object(
  'id', s.id, 'name', s.name, 'building', b.name, 'lat', s.lat, 'lon', s.lon,
  'noise', s.noise, 'live', json_object('est', o.est, 'basis', o.basis))) 
FROM (
  SELECT s.* FROM spot s
  JOIN building_rtree r ON r.id = s.building_id
  WHERE s.status = 'active'
    AND r.min_lon <= :max_lon AND r.max_lon >= :min_lon
    AND r.min_lat <= :max_lat AND r.max_lat >= :min_lat
    AND (s.features & :must_mask) = :must_mask     -- 必须有白板、允许外卖
    AND (s.features & :must_not_mask) = 0
    AND (:max_noise IS NULL OR s.noise <= :max_noise)
  ORDER BY (:w_quiet * COALESCE(s.noise, 2))
         + (:w_dist  * ((s.lat - :lat) * (s.lat - :lat) + (s.lon - :lon) * (s.lon - :lon)))
         + (:w_live  * COALESCE((SELECT est FROM occupancy_live WHERE spot_id = s.id), 1))
  LIMIT 50
) s JOIN building b ON b.id = s.building_id
LEFT JOIN occupancy_live o ON o.spot_id = s.id;
```

- `features` 位图让 20 个布尔条件只是一次整数运算；单校 1000 个 Spot 以内全表扫描也在毫秒级，不需要为每个属性建索引。
- 自由文本搜索（“地下室 插座”）先走 `spot_fts MATCH`，再与上述条件取交集。FTS 里的 `tags` 列存中英文标签文本。
- 距离用经纬度差平方近似排序即可（校园尺度内误差可忽略），返回前再在 C 里用 haversine 算出米数展示。

两个已在 SQLite 3.45.1 上实测的细节：上面的检索语句能正确返回 JSON 数组，但 SQLite 不保证外层 `json_group_array` 保留子查询的 `ORDER BY` 顺序，正式实现时把排序分也输出并在客户端或 C 端保序；`karma_ledger` 的 `UNIQUE` 幂等约束能拦住重复入账，但 SQLite 中 NULL 彼此不相等，所以 `ref_type`/`ref_id` 必须始终填非空值。

## 6. Phase 0：LLM 冷启动管线

结论：一个 Python 命令行工具 `fss-bootstrap`，按 campus 配置文件跑五个可独立重跑的阶段，每阶段产物落盘为 JSONL，最后一阶段写入 SQLite。后端不调用 LLM，也不知道管线存在。

### 6.1 阶段划分

| 阶段 | 输入 | 输出 | 是否调用 LLM |
| --- | --- | --- | --- |
| 1 collect | `campuses/osu.yaml`（种子 URL、subreddit、建筑列表来源） | `raw/*.html`、`raw/*.json` + `docs.jsonl`（url、哈希、抓取时间） | 否 |
| 2 buildings | 官方建筑目录 + OpenStreetMap 建筑轮廓（Overpass API） | `buildings.jsonl`：名称、别名、质心、多边形 | 仅用于别名归一 |
| 3 extract | 清洗后的文档切片 | `mentions.jsonl`：每条是一个“地点 + 属性主张 + 原文引语” | 是（主要成本） |
| 4 resolve | mentions + buildings | `spots.jsonl`：实体对齐到 Building/Spot，合并同一地点的多条主张 | 仅用于歧义裁决 |
| 5 load | spots + mentions | 写入 `campus`/`building`/`spot`/`claim`/`source_doc`，触发物化重算 | 否 |

### 6.2 抓取规则

- 只抓公开页面，遵守 robots.txt，每域名 1 请求/秒，设置可识别的 User-Agent，抓取结果按 URL 哈希缓存，重跑不重抓。
- Reddit 走其官方 API（需要申请应用凭据），按 subreddit 搜索关键词（study spot、quiet place、outlets、library 加各建筑名）。
- Google Maps 评论与 Discord 群组：服务条款不允许爬取，高保真模型**不抓**；在 schema 里保留 `source.kind = maps / discord`，只接受手工或官方 API 导入。这是对商业计划最大的一处删减。
- 平面图 PDF 先用 `pypdf` 抽文字，抽不出来的整页作为 PDF 文档块直接交给 Claude 读。

### 6.3 LLM 抽取设计

- 模型与接口：Anthropic Python SDK，`claude-opus-5-5`，用结构化输出（`client.messages.parse(..., output_format=Pydantic 模型)` 或 `output_config.format`）保证返回合法 JSON。抽取是高并发、不赶时间的批量任务，走 Message Batches API（价格是同步调用的一半），结果按 `custom_id` 对回文档切片。
- 系统提示词固定：包含 `attr_def` 全表与该校建筑别名表，加 `cache_control` 让每条请求复用缓存前缀；每条请求只放一个文档切片（约 2k token）。
- 输出只允许 `attr_def` 里注册过的键，每条主张必须带原文引语 `evidence`，抽取后用字符串包含检查验证引语确实出现在原文中，不在则丢弃（防幻觉的硬门槛）。
- 置信度 `prior` 由规则给出，不让模型自评：官方页面 0.8，Reddit 高赞帖 0.6，普通帖 0.45，超过 3 年的内容乘 0.7。

商业计划里的例子在这套 schema 下的抽取结果：

```json
{"place": {"building": "Bolz Hall", "spot_hint": "basement", "floor": "B"},
 "claims": [
  {"attr": "outlets", "value": 3, "evidence": "墙上全都是插座"},
  {"attr": "late_night", "value": true, "evidence": "期末熬夜绝佳"},
  {"attr": "temp", "value": -2, "evidence": "冷得像冰窖"},
  {"attr": "food_nearby", "value": false, "evidence": "没有卖吃的"},
  {"attr": "vibe", "value": "deep_work", "evidence": "期末熬夜绝佳"}]}
```

### 6.4 标准化空间 schema（v1）

四大块对应商业计划的“坐标、基建、规则、氛围”，全部存在 `attr_def` 中，是扩展到其他学校的唯一契约。

| 分组 | 键 | 类型 |
| --- | --- | --- |
| 坐标 | building, floor, lat, lon, indoor | 结构字段 |
| 基建 | outlets, whiteboard, large\_tables, monitors, printers, wifi\_quality, natural\_light, restroom\_near, accessible, capacity | ordinal / flag |
| 规则 | food\_allowed, calls\_allowed, group\_ok, reservable, hours, access\_card\_required, late\_night | flag / text |
| 氛围 | noise, temp, vibe, crowd\_typical, food\_nearby | ordinal / enum |

新增属性只需在 `attr_def` 加一行（flag 类分配一个空闲位，最多 64 个），管线提示词和前端筛选器都从这张表生成。

### 6.5 实体对齐与入库

1. 建筑对齐：先用别名表精确匹配，再用 rapidfuzz 的 token\_set\_ratio ≥ 90 模糊匹配；都失败才让 LLM 从候选前 5 名中选或判 none。
2. Spot 对齐：同一建筑内按 `(floor, spot_hint)` 归一后分组，同一组合并为一个 Spot；没有具体位置的主张挂到该建筑的“整楼”虚拟 Spot。
3. 入库幂等：`source_doc.url` 唯一，重跑时同一文档的旧 llm 主张先删后插，用户产生的数据不动。
4. 质量门槛：Spot 至少有 2 条来自不同文档的主张才设为 `active`，否则 `hidden`，目标是首校上线时至少 100 个 active Spot。

### 6.6 评估

从抽取结果随机抽 100 条主张人工标注“引语是否支持该值”，准确率低于 90% 就改提示词重跑阶段 3；标注集保留为回归测试集。每次运行记录 token 用量与成本到 `runs/*.json`。

## 7. Phase 1：众包与实时生态

结论：Phase 1 的全部逻辑都是“写一条事件 → 在同一个写事务里更新物化状态与积分 → 事务提交后 publish”。以下每个公式都是可调参数，默认值集中放在 `config/rules.json`，启动时读入。

### 7.1 多维检索

- 请求：`GET /api/v1/spots?bbox=...&must=whiteboard,food_allowed&not=calls_allowed&quiet=2&near=lat,lon&q=地下室`。
- 服务端在启动时把 `attr_def` 读成一张 `FIO_MAP`（键 → 位），把 `must`/`not` 翻译成两个 64 位掩码。`quiet` 是 0–3 的强度，翻译成 `max_noise` 和排序权重 `w_quiet`。
- 前端预置“场景”是筛选组合的别名，不需要服务端支持：如“期末熬夜”= `must=late_night,outlets` + `quiet=2`。

### 7.2 地理围栏判定

客户端随上报附带 `lat, lon, accuracy_m`（浏览器 Geolocation API）。服务端用射线法判断点是否在 `building.fence_json` 多边形内，再用点到多边形边界的最短距离 d 处理定位误差：

| 条件 | 围栏系数 g |
| --- | --- |
| 点在多边形内且 accuracy\_m ≤ 50 | 1.0 |
| 点在外但 d ≤ accuracy\_m 且 accuracy\_m ≤ 100 | 0.6 |
| 其他（包括拒绝定位） | 0.2 |

校园尺度下用等距矩形投影把经纬度转成米再算距离即可。高保真模型承认客户端坐标可伪造：围栏只是加权输入之一，防作弊还靠 7.6 的频率限制与信誉分。

### 7.3 拥挤度估计

每条上报 i 的权重由信誉分 r、围栏系数 g 与时间衰减组成，半衰期默认 20 分钟：

```latex
w_i = r_i \cdot g_i \cdot 2^{-\Delta t_i / 20\,\mathrm{min}}
```

实时估计把预测值当作权重为 w0 = 0.5 的先验伪观测，这样没有新上报时自然回落到预测：

```latex
\hat{L} = \frac{w_0 \cdot L_{\mathrm{forecast}} + \sum_i w_i L_i}{w_0 + \sum_i w_i}, \qquad \mathrm{conf} = 1 - e^{-\sum_i w_i}
```

- 只取近 90 分钟内的上报参与计算。
- `basis` 字段：∑w ≥ 0.5 为 `reports`，否则为 `forecast`，前端据此显示“实时”或“预测”标签。
- 显示颜色：̂L < 0.67 绿，< 1.33 黄，否则红。颜色或 basis 发生变化时才 publish，避免推送抖动。
- 事件类上报（`outlet_broken`、`closed_event`）不参与拥挤度，而是生成一条 TTL 4 小时的临时状态，≥ 2 位独立用户或 1 位 g = 1.0 的高信誉用户确认后展示。

### 7.4 CheckIn

- `POST /checkins` 开启会话，每个用户同时只有一个未结束会话（部分唯一索引 `checkin_open` 保证）。
- 前端每 10 分钟在后台发一次心跳（带坐标），连续 2 次围栏外或 6 小时上限到达则自动结束。
- CheckIn 本身也是拥挤度信号：同一 Spot 的在场人数 / capacity 映射成一条权重 0.3 的隐式上报。
- “学习超过 2 小时”是券触发条件（见第 8 节），以 verified 会话的累计心跳时长为准。

### 7.5 游戏化：积分与徽章

| 贡献 | 积分 | 入账时机 | 每日上限 |
| --- | --- | --- | --- |
| 拥挤度上报（g ≥ 0.6） | +2 | 立即 | 20 次 |
| 事件上报被他人确认 | +5 | 确认时 | — |
| 上传照片 | +3 | 立即；被踩到隐藏则扣回 | 5 张 |
| 属性修正（新 Claim） | +3 | 被 ≥ 2 人顶时 | 10 条 |
| 发现新 Spot | +20 | 进入 active（≥ 2 位其他用户确认）时 | 2 个 |
| verified CheckIn 满 1 小时 | +1 | 会话结束时 | 4 |

- 积分只通过 `karma_ledger` 入账，`user.karma` 是同事务内维护的缓存；幂等键保证重试不重复加分。
- 徽章规则存在 `badge.rule_json`，如 `{"count":"spots_discovered","gte":5}` 即 Campus Explorer；在积分入账后的同一事务里检查并发放，发放后 publish 到 `user:{id}`。

### 7.6 社区审核与信誉分

- 投票权重 = 投票者信誉分；在该建筑有过 verified CheckIn 的投票者权重 × 1.5。
- Claim 的有效置信度用 Beta 分布均值，先验由 `prior` 换算成等效的 5 票：

```latex
p = \frac{5 \cdot \mathrm{prior} + U}{5 + U + D}
```

其中 U、D 是加权顶踩票。同一 `(spot, attr)` 下 p 最高且 p ≥ 0.55 的 Claim 写进 spot 的物化列；都低于 0.55 则该属性显示为“未知”。照片 p < 0.3 且 D ≥ 3 时自动隐藏。

- 信誉分更新：用户的贡献最终被采纳 +0.05，被否决或隐藏 -0.1，限制在 \[0.1, 3.0\]；上报与同时段共识偏差过大（差 ≥ 1.5 且共识 conf ≥ 0.8）计一次否决。更新在 `hourly_rollup` 中批量计算。
- 频率限制：同一用户同一 Spot 10 分钟内只计最后一条上报；每个用户每小时最多在 3 个不同建筑上报（排除“瞬移”）。限制在内存里用一个按 user\_id 分片加锁的 `FIO_MAP` 实现，不入库。

### 7.7 拥挤度预测

- 按 `(spot, day_type, dow, slot)` 对 `occupancy_hourly` 做指数加权平均，近 4 周权重最高；`day_type` 来自 campus 日历的考试周与假期区间。
- 冷启动（样本 n < 5）时用同建筑、同 vibe 的 Spot 的均值当先验；再没有就用全校的时段曲线。
- 课程表信号：如果官方排课数据可抓，把“该楼该时段下课人数”作为一个线性修正项，系数用最小二乘拟合。没有就跳过，不阻塞上线。
- 预测结果输出到 `GET /spots/:id/forecast?date=`，前端画当天的拥挤曲线并标出峰值时段。预测计算在 Python 离线脚本还是 C 定时任务里做均可，默认放 C 里，因为公式简单且让后端自洽。

## 8. Phase 2 与变现

结论：Phase 2 与变现不引入新基础设施，全部建在 Phase 1 的 CheckIn、occupancy 聚合表和 Pub/Sub 之上；唯一新增的硬约束是所有对外数据必须过 k-匿名门槛。

### 8.1 Study With Me

```sql
CREATE TABLE swm_session (
  id INTEGER PRIMARY KEY, host_id INTEGER NOT NULL, spot_id INTEGER NOT NULL,
  course TEXT, topic TEXT, lang TEXT, max_members INTEGER NOT NULL DEFAULT 4,
  starts_at INTEGER NOT NULL, expires_at INTEGER NOT NULL, status TEXT NOT NULL DEFAULT 'open'
);
CREATE TABLE swm_member (session_id INTEGER, user_id INTEGER, joined_at INTEGER, PRIMARY KEY (session_id, user_id)) WITHOUT ROWID;
```

- 发起条件：发起人必须有该 Spot 的 verified CheckIn（保证“人就在这里”）；最长 3 小时。
- 匹配排序：同课程 +3，同专业 +1，语言交集非空 +2，步行距离每 100 米 -0.5，剩余名额为 0 的过滤掉。对应一条 SQL 加 C 端的打分。
- 实时：加入/退出/关闭都 publish 到 `swm:{id}` 与 `spot:{id}`；房间内简单文字消息也走 `swm:{id}` 频道，并用 Pub/Sub 历史缓存做重连补发，不入库。
- 隐私默认值：对外只显示昵称与课程，不显示精确座位；可一键拉黑，被拉黑者看不到发起人的房间。

### 8.2 校园生态热力图

```sql
CREATE TABLE agg_cell (
  campus_id INTEGER, cell TEXT,            -- 按建筑或 ~50m 网格
  hour_ts INTEGER, dim TEXT, dim_value TEXT, -- dim: all / major_group / year
  n_users INTEGER, dwell_min INTEGER,
  PRIMARY KEY (campus_id, cell, hour_ts, dim, dim_value)
) WITHOUT ROWID;
```

- 由 `hourly_rollup` 从 CheckIn 聚合；专业先归到大类（工科、艺术、商科…），不按具体专业出数。
- **k-匿名规则：** `n_users < 5` 的格子在查询时整格丢弃，不显示为 0；热力图最小时间粒度为 1 小时，最近 1 小时不出按专业分组的数据，防止定位到个人。
- 前端用 MapLibre 的 heatmap 图层渲染，数据来自 `GET /api/v1/campus/:slug/heatmap?dim=major_group&from=&to=`。

### 8.3 O2O 券

```sql
CREATE TABLE merchant (id INTEGER PRIMARY KEY, campus_id INTEGER, name TEXT, lat REAL, lon REAL, category TEXT);
CREATE TABLE offer (
  id INTEGER PRIMARY KEY, merchant_id INTEGER, title TEXT,
  trigger_json TEXT NOT NULL,  -- {"checkin_min":120,"within_m":400,"hours":[14,23],"karma_cost":0}
  stock INTEGER NOT NULL, per_user_day INTEGER NOT NULL DEFAULT 1,
  starts_at INTEGER, ends_at INTEGER
);
CREATE TABLE coupon (
  id INTEGER PRIMARY KEY, offer_id INTEGER, user_id INTEGER, code TEXT UNIQUE,
  issued_at INTEGER, redeemed_at INTEGER, checkin_id INTEGER
);
```

- 触发：CheckIn 心跳处理完成后检查该会话累计时长是否跨过某个 offer 的 `checkin_min`，再按距离、时段、库存、每人每日上限过滤；命中后在同一事务里 `UPDATE offer SET stock = stock - 1 WHERE id = ? AND stock > 0` 并插入 coupon，提交后 publish 到 `user:{id}`。
- 积分兑换：`karma_cost > 0` 的 offer 由用户主动兑换，扣分记 `karma_ledger` 负数。
- 核销码：`code = base32(blake2b_hmac(secret, coupon_id || user_id))` 的前 10 位，商家端页面输入核销；引流数据按 offer 统计发放/核销转化率。
- 高保真模型不接真实支付，商家与 offer 用种子数据模拟。

### 8.4 B 端空间利用率报表

- 独立路由 `/api/v1/admin/insights`，需要 user 表新增的 `role = campus_admin`。
- 指标：每楼每时段利用率（在场人数 / 座位估计）、峰谷比、连续低利用时段（可调低 HVAC 的候选）、设施问题排行（`outlet_broken` 等事件按楼计数）。
- 全部来自 `occupancy_hourly` 与 `agg_cell`，同样执行 k=5 门槛；导出 CSV 由 C 直接流式写出（`fio_http_write` 不带 `.finish` 分块写，最后 `fio_http_finish`）。
- 招聘与品牌赞助在技术上就是“按建筑与时段投放的 offer 或横幅”，复用 8.3 的 offer 表（`category = sponsor`），不单独建模。

## 9. API 设计

结论：REST 前缀 `/api/v1`，JSON 进出，会话用 HttpOnly cookie；实时推送只有一个 WebSocket 入口 `/ws`，客户端按频道订阅。

### 9.1 REST 端点

| 方法 路径 | 用途 | 鉴权 |
| --- | --- | --- |
| POST /auth/register、/auth/login、/auth/logout | 注册登录，返回 `fss_sid` cookie | 公开 |
| GET /me、PATCH /me | 个人资料、课程、语言、积分、徽章 | 登录 |
| GET /campuses、GET /campuses/:slug | 校园列表与配置（含 attr\_def） | 公开 |
| GET /spots | 多维检索（7.1） | 公开 |
| GET /spots/:id | 详情：属性、Claim 列表、照片、实时状态 | 公开 |
| GET /spots/:id/forecast?date= | 当天拥挤曲线 | 公开 |
| POST /spots | 提交新 Spot（进入 hidden，待确认） | 登录 |
| POST /spots/:id/reports | 拥挤度或事件上报 | 登录 |
| POST /spots/:id/claims | 属性修正 | 登录 |
| POST /claims/:id/vote、POST /photos/:id/vote | 顶/踩 | 登录 |
| POST /spots/:id/photos | multipart 上传照片 | 登录 |
| POST /checkins、POST /checkins/:id/heartbeat、POST /checkins/:id/end | CheckIn 生命周期 | 登录 |
| GET /swm、POST /swm、POST /swm/:id/join、POST /swm/:id/leave | Study With Me | 登录 |
| GET /campuses/:slug/heatmap | 热力图（k-匿名） | 公开 |
| GET /coupons、POST /offers/:id/redeem | 我的券、积分兑换 | 登录 |
| POST /merchant/coupons/:code/redeem | 商家核销 | merchant |
| GET /admin/insights、GET /admin/insights.csv | B 端报表 | campus\_admin |

错误格式统一为 `{"error":{"code":"rate_limited","message":"..."}}`，状态码按语义（400/401/403/404/409/422/429）。列表端点用 `cursor` 分页。

### 9.2 WebSocket 协议

客户端到服务端：

```json
{"op":"sub","ch":"spot:42","since":1790000000000}
{"op":"unsub","ch":"spot:42"}
{"op":"view","campus":"osu","bbox":[-83.02,40.00,-83.01,40.01]}
{"op":"swm_msg","id":7,"text":"还有一个空位"}
```

服务端到客户端（即 Pub/Sub 消息体原样转发）：

```json
{"t":"live","spot":42,"color":"yellow","est":1.1,"basis":"reports","at":1790000123000}
{"t":"event","spot":42,"kind":"outlet_broken","until":1790014523000}
{"t":"swm","id":7,"kind":"join","members":3}
{"t":"karma","delta":2,"total":118}
{"t":"coupon","offer":"Espresso 20% off","code":"K3M9..."}
```

- `sub` 的频道白名单：`spot:*`、`bldg:*`、`swm:*`（须是成员）；`user:{id}` 在连接建立时自动订阅，客户端不能订阅别人的。
- `view` 用于地图视野：服务端把视野内的建筑换算成 `bldg:{id}` 批量订阅，并退订离开视野的，单连接订阅上限 200。
- `since` 映射到 `replay_since`，用于断线重连后补发。

## 10. 前端、工程化、测试与安全

结论：一个 monorepo，`make dev` 启动 C 后端并由 Vite 开发服务器代理 `/api` 与 `/ws`；发布时前端构建产物放进 `public_folder`，整个系统是一个二进制文件加一个数据库文件。

### 10.1 仓库结构

```
fss/
  server/
    vendor/fio-stl.h  vendor/sqlite3.c
    src/fss_fio.h  src/fio_impl.c  src/main.c
    src/router.c        # 前缀内的 /:id/xxx 匹配
    src/db.c            # 连接、预编译缓存、事务宏
    src/auth.c          # argon2 哈希、会话令牌
    src/geo.c           # 点在多边形、距离
    src/api_spots.c  src/api_reports.c  src/api_checkins.c  src/api_swm.c  src/api_offers.c  src/api_admin.c
    src/live.c          # 拥挤度估计与 publish
    src/jobs.c          # 定时任务
    src/ws.c            # WebSocket 协议
    migrations/*.sql  config/rules.json
    tests/unit/*.c
    Makefile
  pipeline/             # Phase 0，Python
    fss_bootstrap/  campuses/osu.yaml  pyproject.toml
  web/                  # Vite + Preact + MapLibre
  tests/api/            # pytest 黑盒测试
  tests/load/           # k6 脚本
  data/                 # fss.db（git 忽略）与种子数据
```

### 10.2 前端

- 页面：地图首页（筛选抽屉 + 实时色点）、Spot 详情（属性与依据引语、照片、预测曲线、三色上报按钮）、我的（积分、徽章、券）、Study With Me 列表、热力图、商家核销页、校方报表页。
- 筛选器由 `GET /campuses/:slug` 返回的 `attr_def` 动态生成，前端不硬编码属性。
- 地图用 MapLibre GL + OSM 栅格或矢量瓦片，实时色点是一个 GeoJSON source，WebSocket 消息到达时原地更新该 feature 的 `color` 属性。
- 定位：上报与 CheckIn 时调用 `navigator.geolocation.getCurrentPosition({enableHighAccuracy:true})`，把 `coords.accuracy` 一起发送。浏览器要求安全上下文，本地用 `localhost` 即可，手机实测需要 HTTPS（可用 cstl 内置的自签名 TLS）。

### 10.3 构建

| 目标 | 内容 |
| --- | --- |
| `make` | `-O2 -std=gnu11 -Wall -Wextra`，输出 `build/fss` |
| `make debug` | `-O0 -g -DDEBUG -fsanitize=address,undefined` |
| `make test` | 编译并运行 `tests/unit`，再起服务端跑 `pytest tests/api` |
| `make seed` | 跑迁移 + 导入 Phase 0 产物 + 模拟用户与商家 |
| `make dev` | `build/fss` 与 `vite` 并行启动 |

实测：在本环境编译一个展开了 HTTP + FIOBJ 实现的单元，`-O2` 约 22 秒，`-O0` 约 11 秒。这就是 4.1 要求“只在 `fio_impl.c` 一个单元展开实现”的原因：它几乎不改，Makefile 增量编译后业务文件的编译是亚秒级。gcc 13 下 cstl 内部会报几条 `-Wstringop-overflow` 警告，可只对 `fio_impl.c` 关闭。

### 10.4 测试策略

- **规则单元测试（C）**：围栏判定、权重与衰减、Beta 置信度、积分幂等、k-匿名过滤。这些函数设计为纯函数，不碰 IO。
- **API 黑盒测试（pytest）**：每个测试用一个新的临时数据库启动服务端；覆盖每个端点的正常、鉴权失败、限流、并发上报。用一个可注入的时钟（`X-FSS-Now` 头，仅 debug 构建生效）测试衰减与过期逻辑。
- **实时测试**：两个 WebSocket 客户端，一个订阅一个上报，断言收到颜色变化；断线后带 `since` 重连断言补发。
- **压测（k6）**：模拟 2000 个 WebSocket 连接 + 每秒 200 次检索 + 每秒 20 次上报，记录 p99 延迟与 `SQLITE_BUSY` 次数，用来验证 4.2 的线程与锁模型。这是高保真模型的“实验”部分，结果写进文档。
- **模拟器**：Python 脚本生成 N 个虚拟学生，按课表在建筑间移动并上报（含一定比例的作弊者），用来生成预测与热力图所需的历史数据，并验证信誉分能压低作弊者的影响。

### 10.5 安全基线

- 密码：`fio_argon2_hash`（Argon2id），16 字节随机盐。参数在实现时按本机耗时调到约 100 ms。哈希在事务外计算。
- 会话：32 字节随机令牌放 cookie（`HttpOnly; SameSite=Lax`，HTTPS 时加 `Secure`），库里只存 `blake2b(token)`。
- 所有 SQL 均用 `sqlite3_bind_*`，禁止拼接；FTS 查询串先转义为短语查询。
- 上传：限制 5 MB、只接受 JPEG/PNG/WebP（按文件头魔数判断），文件名用 sha256，存在 `public_folder/uploads/`。不在 C 里做图像处理。
- 限流：除 7.6 的业务限制外，按 IP 对 `/auth/*` 做每分钟 10 次限制。
- CSRF：改写类端点要求 `Content-Type: application/json` 并校验 `Origin`。

## 11. 里程碑、风险与待决问题

结论：按 M0–M6 顺序推进，每个里程碑以一个可自动验证的门槛收尾；M1 与 M2 可以并行，因为 M2 可以先用手工种子数据开发。不标日期，节奏由你的实践安排决定。

&#91;embedded content: 实践路线 · 7 个里程碑，每个带门槛\]

### 11.1 风险

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| cstl 0.8.x 仍在快速演进，API 可能变动 | 升级后编译失败 | `vendor/` 固定到具体提交（当前 `24a5701`），升级单独一个提交并跑全量测试 |
| cstl 文档与头文件不一致 | 按文档写出错误代码 | 以头文件为准；已发现一例：`fio_io_run_every` 注释写 `on_stop`，实际字段是 `on_finish` |
| C 里手写 JSON 与路由的工作量 | 进度慢、内存错误 | 列表 JSON 交给 SQLite 生成；ASan 调试构建；规则函数写成纯函数并单测 |
| SQLite 写锁争用 | 高并发上报时 `SQLITE_BUSY` | WAL + `BEGIN IMMEDIATE` + 短事务；压测实测后再决定是否改为单写线程 |
| LLM 抽取幻觉 | 地图上出现错误属性 | 引语必须逐字出现在原文；双来源门槛；人工标注回归集 |
| 数据源条款限制 | Google Maps、Discord 不能爬 | 已从管线中移除，只保留 schema 位置 |
| 没有真实用户 | 预测、热力图、信誉分无法验证 | 用 10.4 的模拟器生成行为数据 |

### 11.2 待决问题

- 首校是否就是 OSU？文档按 OSU 写，换校只改 `campuses/*.yaml`。
- 是否要演示多 worker 进程模式？默认单进程，多进程只需改连接建立点。
- 实践部分你会发到这个线程，收到后我会对照本文档调整里程碑与默认值。

### 参考

- [facil-io/cstl 仓库](https://github.com/facil-io/cstl)（核对版本：master 提交 24a5701，2026-08-21）
- [fio-stl.md](https://github.com/facil-io/cstl/blob/master/fio-stl.md)，以及仓库内 `fio-stl/430 http api.h`、`420 pubsub.h`、`401 io api.h`、`102 queue.h`、`004 state callbacks.h`、`examples/server.c`
