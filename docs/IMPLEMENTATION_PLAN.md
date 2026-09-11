# 实施计划 — REQ-GWM-001

## 当前阶段

本次原型已由用户明确批准并记录，开始及前端门禁均已通过。原生 DLL、独立捕获源、集成测试和易语言资料已实现，正在执行功能与性能验收。用户易语言验收独立记录。

## 阶段出口

1. 工程准备：当前需求的调研记录、GitHub远程可达、Code Start Gate允许。
2. UI原型：单文件PROTOTYPE.html及UI_SPEC.md覆盖全部状态；用户确认后记录 frontend scope `gwm-wall-v1` 审批，通过Frontend Gate。
3. 单窗口验证：HWND+ROI→Canvas，客户区坐标/缩放/DPI正确；多个独立记事本及自带动态源通过。失败先定位，不直接扩展全部DLL。
4. 正式DLL：固定槽位、并发C ABI、WGC和D3D11生命周期、状态查询。
5. 功能/性能：标题、底图、双击、图标、设备恢复；运行TEST_PLAN中自测矩阵。
6. 用户测试：提供易语言声明、样例、测试包与步骤并通知用户；未获得结果时标为待用户验证。

## 构建与发布约束

构建使用MSVC Win32/C++20/静态CRT和Windows SDK，CMake配置Release；产物在out/，成品包在out/packages/。当前机器已发现VS2022 BuildTools与SDK 10.0.26100.0，已通过 x86 Release 与 x64 动态源构建。

每阶段独立提交到codex/gwm-rebuild；不恢复旧src/test实现，不提交旧测试证据作本次证据。保留已有用户工作区删除，不批量stage无关路径。

## 文档同步

需求变更同步REQUIREMENTS/SCOPE；实现变更同步ARCHITECTURE/API_SPEC；测试结果同步TEST_REPORT；原型与真实界面保持一致；CHANGELOG仅记录真实完成状态。PROJECT_CONTEXT由context_refresh生成。
