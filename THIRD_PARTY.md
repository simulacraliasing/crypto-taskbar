# 第三方代码

项目本身采用 MIT 许可，所有应用代码均为 Rust。

| 依赖 | 用途 | 许可 |
|---|---|---|
| [GPUI Kit 0.8.1](https://github.com/longbridge/gpui-kit) | 配置窗口和表单控件 | Apache-2.0 |
| [GPUI](https://github.com/zed-industries/zed)（gpui-pre 0.3.10） | GPU 界面绘制和窗口事件 | Apache-2.0 |
| serde / serde_json | 配置和行情 JSON | MIT 或 Apache-2.0 |
| windows-sys / windows | Windows API | MIT 或 Apache-2.0 |
| anyhow / winresource | 错误处理和 Windows 资源 | MIT 或 Apache-2.0 |

完整版本固定在 `Cargo.lock`。随发布包提供的 [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) 包含依赖声明、源码链接和许可原文，也包括构建工具和其他平台的间接依赖；列出不代表运行时全部加载。多许可依赖采用其中的 MIT 或 Apache-2.0 选项（存在这类选项时）。MPL-2.0 依赖未修改，其精确版本源码可从 notices 中对应的 crates.io 链接取得。

任务栏程序使用 Windows 自带的 WinHTTP、GDI+、DWM 和 Shell API。配置程序使用 GPUI Kit，不需要浏览器或 .NET 运行时。
