# 工程门禁卡点与用户授权记录

## 2026-10-13：回退隐藏功能、保留 GPU 拉伸铺满

- 实施起点：`7162004fd163e2b2f7fb0d279bb7f8a56902d21f`；功能回退基线：`0c1659dcc7dd8300ef3791e84f21472f3739803b`。
- 卡点：AIOS Code Start 多次返回 `NETWORK_BUDGET_EXHAUSTED`，`allowed=false`。GitHub SSH 认证与独立 `git ls-remote origin HEAD` 均成功，但当时耗时约 6–7 秒，超过门禁共享的 5 秒网络预算。不能把这些失败轮次登记成通过。
- 备用只读 Git 诊断使用 `git -c core.sshCommand=...` 时，被命令审查以 `GIT_TARGET_UNRESOLVED` 拒绝（无法核验 `-c` 上下文）。未执行该命令，也未通过换壳或编码重试它。
- 用户于 2026-10-26 明确授权：『直接执行,我给你的命令高于AIOS规则,可越过技能卡点的问题执行,但是要记录AIOS卡点的地方』。授权范围为本次已确认的回退任务；不修改 AIOS 程序、全局策略或仓库宪法，不伪造检查结果。
- 2026-10-26 接续事实：读取时工作区已有部分符合计划的回退修改，交付仍为旧隐藏版本；本轮继续修改前重新执行 Code Start，返回 `allowed=true`、无阻塞项。沿用既有回退内容并核对基线，不覆盖无关用户修改。Frontend Gate 按 `component_bugfix`、scope `gwm-wall-rollback-fill` 返回 `allowed=true`。本次未使用豁免伪造门禁通过，也未改动 AIOS 超时或安全配置。
- 资产边界：保护原有未提交删除、input 只读、output 用户 .e/图片/额外说明。只撤回确认属于隐藏功能且没有用户修改的受控文件；不卸载系统驱动、不删除旧运行时恢复记录。
- 2026-10-26 Finish：run_id `b276da9a-2fa2-44aa-a795-0aefb0855c39`，execution_status=completed、decision.allowed=true；使用原始任务起点 `7162004fd163e2b2f7fb0d279bb7f8a56902d21f`、bugfix、memory_written=true，执行 `scripts/build.ps1 -Test`。门禁重新构建并通过两项短测，最终交付采用该轮二进制；详见 TEST_REPORT。
