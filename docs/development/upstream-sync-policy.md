# 上游同步与 Fork 开发策略（telegram-bot-api）

日期：2026-08-14
状态：生效
规范来源：kt-agent-skills/oss-fork-maintenance

## 仓库角色

- `upstream` = https://github.com/tdlib/telegram-bot-api（只读，禁止 push）
- `origin` = KTAIorg/telegram-bot-api（KT fork）
- 注意：上游默认分支为 `master`，KT 生产线同样使用 `master`（与 deepseek-harness 同例）；
  同步检查用 `upstream/master` 与上游版本提交（上游以 "Update version to X" 提升版本）。

## 分支模型

- `master`：KT 生产线（上游基线 + 白名单定制）
- `upstream-sync`：上游跟随缓冲线
- `gitops-prod`：Argo CD watch（部署清单落地后创建）

## KT 定制边界（白名单，超出需 PR 论证）

1. 容器化：`Dockerfile`（上游无官方镜像，KT 自建）
2. CI/部署：`.github/workflows/`、`deploy/`
3. 治理文档：`docs/`、`AGENTS.md`、`.github/copilot-instructions.md`、`README.md` 顶部 KT 说明段
4. **红线：不改上游 C++/TDLib 源码**（`telegram-bot-api/`、`td/`、`CMakeLists.txt`）。
   如确需源码级改动，须先立项论证并扩充本白名单。

## 同步频率

- 安全修复：即时
- 常规：每上游 release 评估（上游跟随 Bot API 版本节奏发版）；季度至少一次
- 上游发版低频，同步成本低

## 冲突优先级

安全修复以上游为准 > 上游已内建则收敛 KT 定制 > 白名单定制保留 > 风格以上游为准

## 验证清单

- [ ] `docker build` 成功（C++/TDLib 编译耗时长，CI 需配置缓存）
- [ ] 容器以 `--api-id/--api-hash`（+ `--local`）启动后冒烟：`getMe`、`sendMessage`、`getFile`
- [ ] 大文件场景验证：>50MB 上传、无限制下载
- [ ] 上游 release notes 确认是否含安全修复
- [ ] 生产灰度（canary）后全量；bot 切换到本 server 前先在旧 server 执行 `logOut`

## 同步记录

| 日期 | 上游 from→to | 冲突摘要 | 验证 | 操作者 |
|---|---|---|---|---|
| 2026-08-14 | —（采纳基线：adfd7f6 "Update version to 10.2."） | 无 | 建仓、Day 0 文档 | AI Agent |
