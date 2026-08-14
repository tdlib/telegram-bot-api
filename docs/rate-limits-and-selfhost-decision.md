# Telegram Bot API 限流与自建 Server 决策

日期：2026-08-14
状态：生效（决策文档）
主要来源：[Telegram Bots FAQ](https://core.telegram.org/bots/faq)、[tdlib/telegram-bot-api#523](https://github.com/tdlib/telegram-bot-api/issues/523)

## 背景

backlog 原始措辞为「自建 TG Bot API server，绕开官方 30 msg/s 限流」。本文档修正该认知：
**消息限流在 Telegram 服务端执行，自建 Bot API server 无法绕开**；自建解决的是
**文件大小限制**，支撑 KT 的大文件上传场景（>50MB，例如客户把 KT 当云盘使用的场景）。

## 官方限流口径（三层，同时生效）

| 层级 | 限制 | 说明 |
|---|---|---|
| 单个聊天 | 1 条/秒 | 允许短暂 burst，超出返回 429 |
| 单个群 | 20 条/分钟 | 群聊防刷屏 |
| 全局广播 | 约 30 条/秒 | 跨所有 chat 的总量，针对 bulk notifications |

触发后返回 HTTP **429**，响应带 `retry_after` 秒数字段，客户端应按该字段退避重试。

注意：30/秒这一数字主要约束「被动告警 / 批量通知」场景。tdlib 维护者在
[#523](https://github.com/tdlib/telegram-bot-api/issues/523) 中明确：
"Local server has nothing to do with bot limits" —— 限流在 Telegram 后端（DC）侧执行，
走官方 `api.telegram.org` 还是自建 Bot API server，限制完全相同。

### 官方扩容通道：付费广播

BotFather 开启 paid broadcast（`allow_paid_broadcast`）可提升至 1000 msg/s，但超出免费
30/秒的部分每条收 0.1 Telegram Stars，且要求 bot 余额 ≥ 100,000 Stars、月活 ≥ 100,000。
对 KT 当前阶段不适用。

## 自建 Bot API server 真正放宽的限制

上游 README 中 `--local` 模式相对官方 `api.telegram.org` 的差异：

| 项目 | 官方 | 自建（`--local`） |
|---|---|---|
| 文件下载 | ≤ 20 MB | 无大小限制 |
| 文件上传 | ≤ 50 MB | ≤ 2000 MB |
| 上传方式 | multipart / URL | 额外支持本地文件路径与 file URI |
| webhook | HTTPS，端口 443/80/88/8443 | HTTP 可用，任意端口、任意本地 IP |
| `max_webhook_connections` | 1–100，默认 40 | 可配至 100000 |
| `getFile` | 需二次下载 | 直接返回绝对本地路径 |

**没有放宽**：消息限流（1 条/秒 per chat、20 条/分钟 per group、约 30 条/秒全局）一律不变。

## 约束与迁移成本

- 需自备 `api_id` / `api_hash`（my.telegram.org 申请）。
- 同一 bot 不能同时挂在官方 server 与本地 server；切换前先在旧 server 执行 `logOut`，
  否则不保证收到全部 update。
- server 本身只接受 HTTP，远程访问需前置 TLS termination 代理。
- C++/TDLib 编译链重，上游无官方镜像，KT 需自建镜像与 CI（见
  [upstream-sync-policy](development/upstream-sync-policy.md)）。

## 与 KT 体系的结合

1. **出站已收口 `kt-im-telegram-runtime`**：所有发送走 runtime 端点，消费方只持 `bot_key`。
   限流应在 runtime 层做集中式 token bucket（per-chat ~1/s、per-group 20/min、全局预算
   30/s），429 时按 `retry_after` 退避。一处实现，所有消费方受益，且保留审计留痕。
2. **真实风险是群告警撞 20 条/分钟**：cost-guardian 日报、Sentry 告警、escalation 都打进
   同一个运维群时，一波告警风暴会先撞这条线。应做告警聚合合并（edit 更新同一条消息），
   而不是盲目提额。
3. **扩容吞吐用多 bot**：每个 bot 有独立配额，runtime 的 bot 注册表天然支持多 bot，出站
   可按 `bot_key` 分桶。（注意：入站多 bot 有 Hub dedupe 重复消息问题，出站分桶不受影响。）
4. **大文件/云盘场景（本次采纳动机）**：建专用 file bot 挂到自建 server（KT 内网，
   `--local` 模式）；登录/通知等业务 bot 留在官方 `api.telegram.org`，两者互不影响。

## 决策

- 采纳 `tdlib/telegram-bot-api`，fork 为 `KTAIorg/telegram-bot-api`（真 fork）。
- 目标：获得 2000 MB 上传 / 无限制下载能力，支撑大文件与云盘类场景。
- 不改 C++ 内核，定制仅限 CI / 镜像 / 部署 / 文档（白名单见同步策略）。
- backlog 措辞修正为：「自建 TG Bot API server 解决 50MB 上传 / 20MB 下载文件限制
  （云盘场景）；消息限流在 kt-im-telegram-runtime 层集中治理」。

## 参考

- Telegram Bots FAQ（Broadcasting to Users）：https://core.telegram.org/bots/faq
- tdlib 维护者澄清（local server 与 bot 限流无关）：https://github.com/tdlib/telegram-bot-api/issues/523
- 上游 README（`--local` 模式说明）：https://github.com/tdlib/telegram-bot-api
