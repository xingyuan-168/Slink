# 实施计划 — REQ-GWM-001

## 当前任务

用户已批准单命令 DLL 与精简交付方案。保留原型视觉审批，不新增前端流程。将原18个公开命令收敛为 WM_StartMapping；启动参数与可选配置一次传入，关闭后自动回收，需要调整时关闭再启动。

## 交付与验证

只交付 output/GameWindowMapper.dll 与 output/使用说明.md。开发资产保留原目录；用户 output/demo.e 原样整理到 examples/demo.e，不覆盖同名文件。input 只读，旧提交不恢复。

本轮验证仅限单导出、启动显示、关闭释放与重新启动、交付目录；不追加长时间测试。易语言接入由用户测试，未反馈前明确标为待验证。当前实际结果以 TEST_REPORT.md 为准。

## 工程约束

MSVC Win32 / C++20 / 静态CRT；中间构建位于 out。修改接口、生命周期、打包时同步 API_SPEC、ARCHITECTURE、E_LANGUAGE、REQUIREMENTS/SCOPE、测试报告及 CHANGELOG；PROJECT_CONTEXT 由工具生成。按任务提交到 codex/gwm-rebuild，保留已有用户未提交删除，不批量 stage 无关路径。
