# 实施计划 — REQ-GWM-002

## 当前任务

用户已批准虚拟显示器隐藏功能方案，首次安装允许 UAC，关闭画布恢复本次隐藏窗口。保留现有 14 参数单导出，不新增调用命令。新增菜单、初始化与恢复提示需原型审批后实现。

实施起点：0c1659dcc7dd8300ef3791e84f21472f3739803b。2026-10-09 Code Start（major_feature / REQ-GWM-002）已允许；GitHub SSH 连接采用本仓库级 IPv4/443 设置以避免门禁网络超时，未改变远程仓库地址。

顺序：签名驱动核验 → 安装与单窗口 WGC/移动/恢复技术验证 → 用户验收大漠兼容 → 新交互原型批准 → 主 DLL 集成与短测试 → 两文件交付及 Finish。可以并行准备后端模块与审阅原型，不在未批准时实施正式前端。

当前驱动 ZIP、CAT、DLL 及 INF 目录签名已核验；实际设备安装、WGC 联合验证、大漠验收和新增原型审批均未完成。

## 交付与验证

只交付 output/GameWindowMapper.dll 与 output/使用说明.md。驱动和辅助程序内嵌，运行时释放专用目录。开发资产保留仓库；用户 output 中的 .e、图片等本轮不移动或删除。input 只读，既有用户删除不恢复。

本轮验证驱动初始化/失败、窗口移动/恢复、重复 HWND、多宿主、宿主退出和虚拟屏丢失，回归单导出、关闭与重开；不追加长时间压力测试。易语言及大漠绑定由用户验收。当前实际结果以 TEST_REPORT.md 为准。

## 工程约束

MSVC Win32 / C++20 / 静态CRT；中间构建位于 out。修改接口、生命周期、打包时同步 API_SPEC、ARCHITECTURE、E_LANGUAGE、REQUIREMENTS/SCOPE、测试报告及 CHANGELOG；PROJECT_CONTEXT 由工具生成。按任务提交到 codex/gwm-rebuild，保留已有用户未提交删除，不批量 stage 无关路径。
