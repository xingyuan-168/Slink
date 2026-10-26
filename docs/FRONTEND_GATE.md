# Frontend Gate — 普通画布回退与 GPU 铺满

scope: gwm-wall-rollback-fill
status: approved

2026-10-13 用户明确选择回退隐藏功能且保留拉伸铺满，并批准完整实施计划。2026-10-26 接续实施前，Frontend Gate 以 component_bugfix 返回 allowed=true：恢复已批准的普通画布和关闭释放，保留已批准的铺满行为，不增加交互。当前原型和 UI_SPEC 已移除虚拟屏菜单及初始化／恢复流程。

## REQ-GWM-002 / gwm-virtual-display-v1（历史，已撤回）

2026-10-10：用户明确回复“批准新版原型，继续实现”，同时选择“先接入正式 DLL，再由我验收”。新增右键隐藏/显示、驱动初始化、关闭恢复/失败重试及拉伸铺满已批准。approval_record 已记录本 scope；Frontend Gate 允许后进入正式画布实现。

## 历史审批 REQ-GWM-001 / gwm-wall-v1

2026-09-11 用户明确表示“原型批准.执行开发”。approval_record 已记录 APPROVAL-20260911081646070350，Frontend Gate 返回 allowed=true。本次批准针对当前 PROTOTYPE.html / UI_SPEC.md，不沿用历史批准。

批准范围：固定横向网格、顶部标题和编号、原始 LOGO、等比 ROI、无画面状态、双击激活、缩放行为。右侧参数区和模拟反馈只属于原型审阅工具，原生画布不附加该参数面板。
